import Combine
import CoreLocation
import FerrostarCore
import FerrostarCoreFFI
import Foundation
import Network
import Observation
import QuartzCore

/// Turn-by-turn guidance: Valhalla plans the route, Ferrostar follows the GPS along it,
/// and every change of "next turn / distance / road" is sent to the pod.
@MainActor
@Observable
final class Navigator {
    /// Free public Valhalla server run by FOSSGIS. Fine for light personal use only;
    /// switch to a hosted provider (e.g. Stadia Maps) or self-host if usage grows.
    static let valhallaURL = "https://valhalla1.openstreetmap.de/route"

    enum Phase: Equatable {
        case idle
        case routing
        case preview(Route)       // route found, shown on the map, not started yet
        case navigating
        case arrived
        case failed(String)
    }

    private(set) var phase: Phase = .idle
    private(set) var destinationName = ""
    /// True when the previewed route starts somewhere other than the rider (chosen in the app):
    /// such a route can only be simulated, since real guidance follows the phone's own GPS.
    private(set) var routeStartsElsewhere = false
    private(set) var instruction = ""
    private(set) var distanceToTurn = 0
    private(set) var remainingKm = 0.0
    /// Off the route, or fetching a new one
    private(set) var rerouting = false
    /// Full Ferrostar state, for the in-app navigation map
    private(set) var navState: NavigationState?
    /// Voice guidance: muted unless the rider turned it on (the choice is remembered)
    private(set) var isMuted = UserDefaults.standard.object(forKey: "voiceMuted") as? Bool ?? true
    /// Stops the automatic pod updates, e.g. while the manual "Pod test" sheet is open
    @ObservationIgnored var podFeedPaused = false
    /// Replay routes with fake GPS instead of following the phone (set in the Pod & testing sheet).
    /// On at launch in development (Debug) builds, as asked while the app is being built; off in
    /// release builds, so a real ride never starts as a simulation by mistake.
    #if DEBUG
    var simulateRides = true
    #else
    var simulateRides = false
    #endif
    var simulationSpeed: UInt64 = 4
    /// The finished trip, for the Arrived card
    private(set) var lastTrip: (km: Double, seconds: TimeInterval)?
    /// The arrow on the phone's navigation map: on the route, pointing along it (see RoutePuck)
    @ObservationIgnored private var routePuck = RoutePuck()
    @ObservationIgnored private var puckMotion: PuckMotion?
    @ObservationIgnored private var lastFixTime: CFTimeInterval = 0
    /// Ferrostar's simulated positions carry no speed (it is always empty), so the pod got 0 and
    /// its lateral g (speed x turn rate) and speed displays never moved: worked out here instead
    @ObservationIgnored private var simulating = false
    @ObservationIgnored private var simulatedSpeed = 0.0                       // metres per second
    @ObservationIgnored private var lastSimulated: (location: CLLocation, at: Date)?
    /// Past rides; the one in progress is recorded as it goes
    let history = RideHistory()
    @ObservationIgnored private var recorder: RideRecorder?

    /// Updated as fixes arrive (reading the provider directly didn't refresh the screen)
    private(set) var hasGPS = false
    var currentCoordinate: CLLocationCoordinate2D? { device?.lastLocation?.clLocation.coordinate }

    private let link: PodLink
    /// Created by activate(), after the first screen is up: setting it up asks the system for the
    /// location permission and waits for the answer, which held the whole UI at launch
    @ObservationIgnored private var device: FreshLocationProvider?
    @ObservationIgnored private var gpsWatch: AnyCancellable?
    private let simulated = SimulatedLocationProvider()
    private var core: FerrostarCore?
    @ObservationIgnored private var stateWatch: AnyCancellable?
    /// Last line sent per kind ("turn", "path", ...) and when, for change detection and keepalive
    @ObservationIgnored private var lastPodSent: [String: (line: String, at: Date)] = [:]
    @ObservationIgnored private var turnLine = ""          // latest "R|400|MG Rd" while navigating
    @ObservationIgnored private var tripLine = ""          // latest "TRIP|18:42|6.0"
    @ObservationIgnored private var pathLine = ""          // latest "RPATH|..." route for the pod's map
    @ObservationIgnored private var poseLine = ""          // latest "POSE|..." rider in the map area
    @ObservationIgnored private var mapSentForConnection = -1
    /// The connection the lines in lastPodSent went over: a new one (the pod restarted after the
    /// bike's power came back, or reconnected) gets everything again at once
    @ObservationIgnored private var linesSentForConnection = -1
    private let mapFeed = MapFeed()
    private let hazards = Hazards()
    @ObservationIgnored private var limitLine = ""         // latest "LIMIT|50" (0 = not known here)
    @ObservationIgnored private var bumpLine = ""          // latest "BUMP|85" (0 = none close)
    /// Sees Valhalla's raw answers on their way into Ferrostar, to keep the junction details
    private let junctionCapture = JunctionCapture()
    @ObservationIgnored private var pathAlong: Double?     // rider's last position along the route, metres
    @ObservationIgnored private var pathRouteCount = 0     // detects a new route (reroute) to reset pathAlong
    @ObservationIgnored private var tripStartedAt = Date()
    @ObservationIgnored private var tripKm = 0.0
    @ObservationIgnored private var podFeed: Task<Void, Never>?
    @ObservationIgnored private var routeRequestID = UUID()
    @ObservationIgnored private var navStatesLogged = 0
    @ObservationIgnored private var lastLoggedManeuver = ""
    /// The pod keeps "Rerouting" up at least this long: the new route often comes back in under a
    /// second, between two pod updates, and the screen never showed
    @ObservationIgnored private var rerouteHeldUntil = Date.distantPast
    /// Whether the phone has internet, to say why no new route is coming
    @ObservationIgnored private let network = NWPathMonitor()
    @ObservationIgnored private var online = true

    init(link: PodLink) {
        self.link = link
        network.pathUpdateHandler = { [weak self] path in
            let online = path.status == .satisfied
            Task { @MainActor in self?.online = online }
        }
        network.start(queue: .global(qos: .utility))
        // A pod that has just (re)connected, e.g. restarted when the bike's power came back:
        // bring it up to date now rather than at the next 1 s update
        link.onConnected { [weak self] in self?.updatePod() }
        // The pod's deepest lean in the last 2 s, for the ride history
        link.onEvent { [weak self] line in
            let fields = line.split(separator: "|")
            guard fields.count == 3, fields[0] == "LEAN", let left = Int(fields[1]), let right = Int(fields[2]) else { return }
            self?.recorder?.lean(left: left, right: right)
        }
        podFeed = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                self.updatePod()
                try? await Task.sleep(for: .seconds(1))
            }
        }
    }

    /// Starts following the phone's GPS (call once the first screen is showing)
    func activate() {
        guard device == nil else { return }
        let device = FreshLocationProvider(activityType: .automotiveNavigation, allowBackgroundLocationUpdates: true)
        device.startUpdating()   // also triggers the location permission prompt
        self.device = device
        gpsWatch = device.$lastLocation
            .receive(on: DispatchQueue.main)
            .sink { [weak self] location in
                if self?.hasGPS != (location != nil) { self?.hasGPS = location != nil }
            }
    }

    private static let config = SwiftNavigationControllerConfig(
        waypointAdvance: .waypointWithinRange(100.0),
        stepAdvanceCondition: stepAdvanceDistanceEntryAndExit(
            distanceToEndOfStep: 30,
            distanceAfterEndOfStep: 5,
            minimumHorizontalAccuracy: 32
        ),
        arrivalStepAdvanceCondition: stepAdvanceDistanceToEndOfStep(
            distance: 10,
            minimumHorizontalAccuracy: 32
        ),
        routeDeviationTracking: .staticThreshold(minimumHorizontalAccuracy: 25, maxAcceptableDeviation: 50),
        snappedLocationCourseFiltering: .snapToRoute
    )

    private func makeCore(_ provider: LocationProviding) throws -> FerrostarCore {
        try FerrostarCore(
            wellKnownRouteProvider: .valhalla(endpointUrl: Self.valhallaURL, profile: "motorcycle"),
            locationProvider: provider,
            navigationControllerConfig: Self.config,
            networkSession: junctionCapture
        )
    }

    /// Step 1: ask Valhalla for a motorcycle route from here and show it.
    /// - Parameter start: where the route starts; nil = the phone's current position
    func planRoute(from start: CLLocationCoordinate2D? = nil, to destination: CLLocationCoordinate2D, name: String) async {
        stop()
        let origin: UserLocation
        if let start {
            origin = UserLocation(clCoordinateLocation2D: start)
        } else if let here = device?.lastLocation {
            origin = here
        } else {
            phase = .failed("No GPS position yet. Wait a few seconds and try again.")
            return
        }
        routeStartsElsewhere = start != nil
        destinationName = name
        phase = .routing
        let requestID = UUID()
        routeRequestID = requestID
        do {
            let router = try makeCore(device ?? simulated)   // only asks for routes: no GPS needed
            let routes = try await router.getRoutes(
                initialLocation: origin,
                waypoints: [Waypoint(coordinate: GeographicCoordinate(lat: destination.latitude,
                                                                       lng: destination.longitude),
                                     kind: .break)]
            )
            guard routeRequestID == requestID else { return }   // cancelled or replaced meanwhile
            guard let route = routes.first else {
                phase = .failed("No route found to that destination.")
                return
            }
            phase = .preview(route)
            DebugLog.write(String(format: "PLAN from %.6f,%.6f (acc %.0f m); route starts %.6f,%.6f",
                                  origin.coordinates.lat, origin.coordinates.lng, origin.horizontalAccuracy,
                                  route.geometry.first?.lat ?? 0, route.geometry.first?.lng ?? 0))
            // Fetch the map tiles around the start now, so the pod's map is ready the moment
            // guidance starts (it showed only the route until they had downloaded)
            if let first = route.geometry.first { mapFeed.prefetch(lat: first.lat, lng: first.lng) }
        } catch {
            guard routeRequestID == requestID else { return }
            phase = .failed(Self.friendlyMessage(error))
        }
    }

    /// True when starting the previewed route would replay it instead of following the GPS
    var willSimulate: Bool { simulateRides || routeStartsElsewhere }

    /// Step 2: start guidance on the previewed route (simulated if `willSimulate`;
    /// `simulationSpeed` 1 = about 36 km/h)
    func startNavigation() {
        guard case let .preview(route) = phase else { return }
        let simulate = willSimulate
        do {
            guard let provider: LocationProviding = simulate ? simulated : device else {
                phase = .failed("No GPS yet. Wait a few seconds and try again.")
                return
            }
            let core = try makeCore(provider)
            if simulate {
                simulated.warpFactor = max(1, simulationSpeed)
                // One simulated GPS point every 10 m, once per second (divided by the speed-up)
                try simulated.setSimulatedRoute(route, resampleDistance: 10)
            }
            if core.spokenInstructionObserver.isMuted != isMuted {
                core.spokenInstructionObserver.toggleMute()
            }
            self.core = core
            stateWatch = core.$state
                .receive(on: DispatchQueue.main)
                .sink { [weak self] state in self?.handle(state) }
            DebugLog.write(String(format: "START simulate=%d provider last %.6f,%.6f acc %.0f", simulate ? 1 : 0,
                                  provider.lastLocation?.coordinates.lat ?? 0, provider.lastLocation?.coordinates.lng ?? 0,
                                  provider.lastLocation?.horizontalAccuracy ?? -1))
            navStatesLogged = 0
            try core.startNavigation(route: route)
            tripStartedAt = Date()
            tripKm = route.distance / 1000
            lastTrip = nil
            recorder = RideRecorder(destination: destinationName, simulated: simulate)
            simulating = simulate
            simulatedSpeed = 0
            lastSimulated = nil
            turnLine = ""
            tripLine = ""
            pathLine = ""
            poseLine = ""
            pathAlong = nil
            phase = .navigating
        } catch {
            phase = .failed("Could not start navigation: \(error.localizedDescription)")
        }
    }

    func toggleMute() {
        setMuted(!isMuted)
    }

    func setMuted(_ muted: Bool) {
        isMuted = muted
        UserDefaults.standard.set(isMuted, forKey: "voiceMuted")
        if let observer = core?.spokenInstructionObserver, observer.isMuted != isMuted {
            observer.toggleMute()
        }
    }

    func stop() {
        finishRide(arrived: false)           // ended before arriving (after arriving it's already saved)
        routeRequestID = UUID()
        stateWatch = nil
        core?.stopNavigation()
        core = nil
        navState = nil
        turnLine = ""
        tripLine = ""
        pathLine = ""
        poseLine = ""
        limitLine = ""
        bumpLine = ""
        hazards.clear()
        rerouting = false
        rerouteHeldUntil = .distantPast
        puckMotion = nil
        routePuck = RoutePuck()
        simulating = false
        simulatedSpeed = 0
        lastSimulated = nil
        if phase != .idle { phase = .idle }
        updatePod()
    }

    private func handle(_ state: NavigationState?) {
        navState = state
        guard let state else { return }
        if navStatesLogged < 15 {
            navStatesLogged += 1
            let p = state.preferredUserLocation
            DebugLog.write(String(format: "STATE %d navigating=%d user %.6f,%.6f acc %.0f", navStatesLogged, state.isNavigating ? 1 : 0,
                                  p?.coordinates.lat ?? 0, p?.coordinates.lng ?? 0, p?.horizontalAccuracy ?? -1))
        }
        // Off the route entirely (Ferrostar asks for a new route at most every 5 s and only after
        // moving 50 m, and keeps the old one if the request fails), or already fetching one
        var offRoute = false
        if case .deviation(kind: .completelyOffRoute) = state.currentDeviation { offRoute = true }
        let nowRerouting = state.isCalculatingNewRoute || offRoute
        if nowRerouting != rerouting {
            DebugLog.write("REROUTE \(nowRerouting ? "start" : "done") (fetching \(state.isCalculatingNewRoute ? 1 : 0), off route \(offRoute ? 1 : 0))")
        }
        rerouting = nowRerouting
        if rerouting { rerouteHeldUntil = Date().addingTimeInterval(2) }

        if case .complete = state.tripState {
            if phase != .arrived {
                // The distance actually ridden (detours included), else the route's
                let ridden = finishRide(arrived: true)?.km ?? 0
                lastTrip = (ridden > 0 ? ridden : tripKm, Date().timeIntervalSince(tripStartedAt))
            }
            phase = .arrived
            updatePod()
            return
        }
        if case let .navigating(_, raw, _, _, _, _, _, _, _, _, _) = state.tripState, simulating {
            let here = CLLocation(latitude: raw.coordinates.lat, longitude: raw.coordinates.lng), now = Date()
            // Only on a new position: states also change without the rider moving
            if let last = lastSimulated {
                let moved = here.distance(from: last.location), seconds = now.timeIntervalSince(last.at)
                if moved > 0.5, seconds > 0.05 {
                    simulatedSpeed += (moved / seconds - simulatedSpeed) * 0.5
                    lastSimulated = (here, now)
                }
            } else {
                lastSimulated = (here, now)
            }
        }
        if let location = state.preferredUserLocation {
            recorder?.add(location, speedOverride: simulating ? simulatedSpeed : nil)
        }
        if case let .navigating(_, raw, _, _, _, _, _, _, _, _, _) = state.tripState,
           let placed = routePuck.place(raw, on: state.routeGeometry) {
            // Slide from where the arrow is now to the new fix, over the time the last fix took to
            // come (states also change without a new fix: those leave the slide alone)
            let now = CACurrentMediaTime()
            if placed.along != puckMotion?.to || placed.newRoute {
                let shown = puckMotion?.along(at: now) ?? placed.along
                let gap = lastFixTime > 0 ? min(max(now - lastFixTime, 0.1), 1.5) : 0
                let jump = placed.newRoute || abs(placed.along - shown) > 150
                puckMotion = PuckMotion(from: jump ? placed.along : shown, to: placed.along, start: now,
                                        duration: jump ? 0 : gap, horizontalAccuracy: raw.horizontalAccuracy,
                                        speed: simulating ? simulatedSpeed : max(0, raw.speed?.value ?? 0))
                lastFixTime = now
            }
        } else {
            puckMotion = nil
        }
        guard state.isNavigating,
              let progress = state.currentProgress,
              let content = state.currentVisualInstruction?.primaryContent
        else { return }

        instruction = content.text
        distanceToTurn = Int(progress.distanceToNextManeuver.rounded())
        remainingKm = progress.distanceRemaining / 1000

        let road = Self.podText(Self.shortRoadName(content.text))
        if let degrees = content.roundaboutExitDegrees, Self.isRoundabout(content.maneuverType) {
            // O|metres|exit|degrees|road. The maneuver point is where the current step ends
            let point = state.currentStep?.geometry.last
            let exit = point.flatMap { junctionCapture.roundaboutExit(at: $0.lat, $0.lng) } ?? 0
            // Degrees are measured in the driving direction: clockwise where traffic keeps left (India)
            let right = point.map { junctionCapture.keepsRight(at: $0.lat, $0.lng) } ?? false
            turnLine = "O|\(distanceToTurn)|\(exit)|\(right ? -Int(degrees) : Int(degrees))|\(road)"
        } else {
            let dir = Self.podDirection(type: content.maneuverType, modifier: content.maneuverModifier)
            turnLine = "\(dir.rawValue)|\(distanceToTurn)|\(road)"
        }

        // Log each new maneuver (not every distance update)
        let maneuverKey = turnLine.split(separator: "|").enumerated().filter { $0.offset != 1 }.map(\.element).joined(separator: "|")
        if maneuverKey != lastLoggedManeuver {
            lastLoggedManeuver = maneuverKey
            DebugLog.write("TURN \(turnLine) (type \(String(describing: content.maneuverType)), modifier \(String(describing: content.maneuverModifier)), degrees \(content.roundaboutExitDegrees.map(String.init) ?? "-"))")
        }

        let newLimit = "LIMIT|\(Self.speedLimit(state.currentAnnotationJSON) ?? 0)"
        if newLimit != limitLine {
            DebugLog.write("\(newLimit) (annotation \(state.currentAnnotationJSON?.prefix(120) ?? "none"))")
        }
        limitLine = newLimit

        let eta = Date().addingTimeInterval(progress.durationRemaining)
        // Arrival time, km left, the route's total and seconds riding (for the pod's journey screen)
        tripLine = String(format: "TRIP|%@|%.1f|%.1f|%d", Self.clockFormat.string(from: eta), remainingKm,
                          tripKm, Int(Date().timeIntervalSince(tripStartedAt)))

        if state.routeGeometry.count != pathRouteCount {   // new route: forget the old position
            pathRouteCount = state.routeGeometry.count
            hazards.load(route: state.routeGeometry)
            pathAlong = nil
        }
        let junctions = core?.route.map { junctionCapture.junctions(forRouteDistance: $0.distance) } ?? []
        if let rider = state.preferredUserLocation?.coordinates,
           let result = PathShape.around(rider: rider, route: state.routeGeometry,
                                         metresToTurn: progress.distanceToNextManeuver, previousAlong: pathAlong,
                                         junctions: junctions) {
            pathAlong = result.along
            // Next mapped speed breaker, if one is close
            let toBump = hazards.next(after: result.along)
            bumpLine = "BUMP|\(toBump.map { $0 <= 200 ? Int($0.rounded()) : 0 } ?? 0)"
            // Route for the pod's map, in the map area's fixed frame (whole metres from the area's
            // reference point): the same route gives the same numbers every update, so it doesn't
            // wobble the way points relative to the moving rider did
            if let frame = mapFeed.frame {
                let o = frame.project(result.origin.lat, result.origin.lng)
                var points = result.northUp.map { "\(Int((o.x + $0.x).rounded())),\(Int((o.y + $0.y).rounded()))" }
                // One line must fit the pod's 512 bytes: drop points from the far end if needed
                while points.count > 2, points.joined(separator: ";").utf8.count > 480 { points.removeLast() }
                pathLine = "RPATH|\(mapFeed.areaID)|" + points.joined(separator: ";")
            }
            // Street map: same origin and heading as the route, so they line up
            let origin = result.origin
            poseLine = mapFeed.pose(lat: origin.lat, lng: origin.lng, headingDegrees: result.headingDegrees) ?? ""
            if mapFeed.needsArea(lat: origin.lat, lng: origin.lng) {
                mapFeed.buildArea(lat: origin.lat, lng: origin.lng) { [weak self] lines in self?.sendMapArea(lines) }
            }
        }
        updatePod()
    }

    /// The arrow on the phone's navigation map at `time` (CACurrentMediaTime), on the route and
    /// pointing along it; nil when off the route or not navigating (see RoutePuck)
    func puckLocation(at time: CFTimeInterval) -> CLLocation? {
        guard phase == .navigating, let motion = puckMotion else { return nil }
        return routePuck.location(at: motion.along(at: time), horizontalAccuracy: motion.horizontalAccuracy,
                                  speed: motion.speed)
    }

    @discardableResult
    private func finishRide(arrived: Bool) -> Ride? {
        guard let recorder else { return nil }
        self.recorder = nil
        let ride = recorder.finish(arrived: arrived)
        history.save(ride, track: recorder.track)
        return ride
    }

    private static let clockFormat: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm"
        return f
    }()

    /// GPS speed for the pod's lean estimate, in tenths of a metre per second (simulated rides
    /// report the simulated speed)
    private var speedLine: String {
        let location = phase == .navigating ? navState?.preferredUserLocation : device?.lastLocation
        let metresPerSecond = phase == .navigating && simulating ? simulatedSpeed : max(0, location?.speed?.value ?? 0)
        return "SPD|\(Int((metresPerSecond * 10).rounded()))"
    }

    /// What the pod should be showing right now, as (kind, line) pairs (see BLE_Link.h on the pod).
    private var podLines: [(kind: String, line: String)] {
        [("clock", timeLine), ("speed", speedLine)] + screenLines
    }

    /// The phone's local time, to set the pod's clock (it keeps time when the phone is away)
    private var timeLine: String {
        let c = Calendar.current.dateComponents([.hour, .minute, .second], from: Date())
        return String(format: "TIME|%02d|%02d|%02d", c.hour ?? 0, c.minute ?? 0, c.second ?? 0)
    }

    private var screenLines: [(kind: String, line: String)] {
        switch phase {
        case .navigating:
            if rerouting || Date() < rerouteHeldUntil {
                let detail = rerouting && !online ? "No internet" : "Finding a new route"
                return [("state", "REROUTE|\(detail)")] + (tripLine.isEmpty ? [] : [("trip", tripLine)])
            }
            guard !turnLine.isEmpty else { return [] }   // first instruction not computed yet
            // Turn first (the pod zooms from the turn distance), poses before the route (the pod
            // anchors the route to the latest pose)
            return [("turn", turnLine), ("pose", poseLine), ("path", pathLine), ("trip", tripLine),
                    ("limit", limitLine), ("bump", bumpLine)]
                .filter { !$0.line.isEmpty }
        case .arrived:
            let minutes = Int((Date().timeIntervalSince(tripStartedAt) / 60).rounded())
            let place = Self.podText(destinationName)
            return [("state", String(format: "ARRIVED|%.1f|%d|", tripKm, minutes) + place)]
        case .idle, .routing, .preview, .failed:
            let metresPerSecond = max(0, device?.lastLocation?.speed?.value ?? 0)
            let speed = Int((metresPerSecond * 3.6).rounded())
            return [("state", "CRUISE|\(speed)|\(Self.clockFormat.string(from: Date()))")]
        }
    }

    /// Sends each line when it changes, and repeats it every 2 s as a keepalive:
    /// the pod fades its screen if it hears nothing for 8 s.
    /// Route drawings go at most 4 times a second so a fast simulation can't flood the link.
    /// A whole map area: ~100-250 lines, sent once per area (and again after a reconnect)
    private func sendMapArea(_ lines: [String]) {
        guard !podFeedPaused, link.state == .connected else { return }
        // MAPBEGIN (the area's reference point) goes with the live lines: poses and the route switch
        // to the new area's frame at once, and the pod can't place them without it. Queued in bulk
        // behind them, it came late and the pod's arrow left the route at bends for a moment.
        for (i, line) in lines.enumerated() { link.send(line: line, priority: i == 0 ? .live : .bulk) }
        mapSentForConnection = link.connectionID
    }

    private func updatePod() {
        guard !podFeedPaused else { return }
        if phase == .navigating, !mapFeed.areaLines.isEmpty, mapSentForConnection != link.connectionID {
            sendMapArea(mapFeed.areaLines)          // the pod reconnected (or restarted): it has no map
        }
        if link.connectionID != linesSentForConnection {
            linesSentForConnection = link.connectionID
            lastPodSent.removeAll()
        }
        let now = Date()
        for (kind, line) in podLines {
            let last = lastPodSent[kind]
            let age = now.timeIntervalSince(last?.at ?? .distantPast)
            // The time changes every second: send it on connecting and then every 30 s
            if kind == "clock", let last, last.at > link.connectedAt, age < 30 { continue }
            let changed = line != last?.line
            let throttled = kind == "path" || kind == "stub" || kind.hasPrefix("pose") || kind == "speed" || kind == "bump"
            guard (changed && (!throttled || age >= 0.25)) || age >= 2 else { continue }
            lastPodSent[kind] = (line, now)
            link.send(line: line, kind: kind)
        }
    }

    /// The speed limit (km/h) where the rider is, from the route's per-segment data (OSM maxspeed);
    /// nil where the map doesn't say
    static func speedLimit(_ annotationJSON: String?) -> Int? {
        guard let data = annotationJSON?.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let maxspeed = object["maxspeed"] as? [String: Any],
              let speed = maxspeed["speed"] as? Double, speed > 0
        else { return nil }
        let kmh = (maxspeed["unit"] as? String) == "mph" ? speed * 1.609 : speed
        return Int(kmh.rounded())
    }

    /// A routing error in plain words
    static func friendlyMessage(_ error: Error) -> String {
        let ns = error as NSError
        if ns.domain == NSURLErrorDomain {
            switch URLError.Code(rawValue: ns.code) {
            case .notConnectedToInternet, .networkConnectionLost, .dataNotAllowed, .internationalRoamingOff:
                return "No internet connection. Check your signal and try again."
            case .timedOut, .cannotConnectToHost, .cannotFindHost, .dnsLookupFailed:
                return "The route server didn't answer. Try again in a moment."
            default:
                break
            }
        }
        return "Couldn't get a route. \(error.localizedDescription)"
    }

    private static func isRoundabout(_ type: ManeuverType?) -> Bool {
        switch type {
        case .roundabout, .rotary, .roundaboutTurn, .exitRoundabout, .exitRotary: true
        default: false
        }
    }

    /// The pod only draws left / right / straight arrows (and roundabouts, above).
    private static func podDirection(type: ManeuverType?, modifier: ManeuverModifier?) -> TurnDir {
        if type == .arrive { return .straight }
        switch modifier {
        case .left, .slightLeft, .sharpLeft: return .left
        case .right, .slightRight, .sharpRight: return .right
        case .uTurn: return .right   // India drives on the left, so U-turns swing right
        case .straight, .none: return .straight
        }
    }

    /// Word endings shortened the way car navigators do, so names fit the pod's two lines.
    private static let abbreviations = [
        "Road": "Rd", "Street": "St", "Avenue": "Ave", "Highway": "Hwy", "Junction": "Jn", "Nagar": "Ngr",
    ]

    /// "Outer Ring Road; KC Reddy Ring Road" -> "Outer Ring Rd"
    static func shortRoadName(_ name: String) -> String {
        // OSM can list several names for one road, separated by ";": keep the first
        var short = name.split(separator: ";").first.map(String.init) ?? name
        for (long, abbr) in abbreviations {
            short = short.replacingOccurrences(of: "\\b\(long)\\b", with: abbr, options: .regularExpression)
        }
        // Unnamed roads come through as an instruction ("Turn left."): drop the full stop
        return short.trimmingCharacters(in: .whitespaces.union(CharacterSet(charactersIn: ".")))
    }

    /// The pod's fonts only contain plain ASCII characters: other scripts are written in Latin
    /// letters and accents dropped ("Hosūr" -> "Hosur", Kannada names -> their Latin spelling)
    /// instead of losing those letters
    static func podText(_ text: String) -> String {
        var latin = text.applyingTransform(.toLatin, reverse: false) ?? text
        // Names written only in another script come out lower case: capitalise them
        if !text.unicodeScalars.contains(where: { $0.isASCII && CharacterSet.letters.contains($0) }) {
            latin = latin.capitalized
        }
        var plain = latin.applyingTransform(.stripDiacritics, reverse: false) ?? latin
        for (fancy, simple) in [("\u{2013}", "-"), ("\u{2014}", "-"), ("\u{2018}", "'"), ("\u{2019}", "'"),
                                ("\u{201C}", "\""), ("\u{201D}", "\"")] {
            plain = plain.replacingOccurrences(of: fancy, with: simple)
        }
        return String(String.UnicodeScalarView(plain.unicodeScalars.filter { $0.isASCII && $0 != "|" }))
    }
}
