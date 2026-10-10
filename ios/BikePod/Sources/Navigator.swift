import Combine
import CoreLocation
import FerrostarCore
import FerrostarCoreFFI
import Foundation
import Observation

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
    private(set) var rerouting = false
    /// Full Ferrostar state, for the in-app navigation map
    private(set) var navState: NavigationState?
    private(set) var isMuted = false
    /// Stops the automatic pod updates, e.g. while the manual "Pod test" sheet is open
    @ObservationIgnored var podFeedPaused = false

    var hasGPS: Bool { device.lastLocation != nil }
    var currentCoordinate: CLLocationCoordinate2D? { device.lastLocation?.clLocation.coordinate }

    private let link: PodLink
    private let device = CoreLocationProvider(activityType: .automotiveNavigation,
                                              allowBackgroundLocationUpdates: true)
    private let simulated = SimulatedLocationProvider()
    private var core: FerrostarCore?
    @ObservationIgnored private var stateWatch: AnyCancellable?
    /// Last line sent per kind ("turn", "path", ...) and when, for change detection and keepalive
    @ObservationIgnored private var lastPodSent: [String: (line: String, at: Date)] = [:]
    @ObservationIgnored private var turnLine = ""          // latest "R|400|MG Rd" while navigating
    @ObservationIgnored private var tripLine = ""          // latest "TRIP|18:42|6.0"
    @ObservationIgnored private var pathLine = ""          // latest "RPATH|..." route for the pod's map
    @ObservationIgnored private var poseLine = ""          // latest "POSE|..." rider in the map area
    @ObservationIgnored private var previousPoseLine = ""  // same, in the previous area (see MapFeed)
    @ObservationIgnored private var mapSentForConnection = -1
    private let mapFeed = MapFeed()
    /// Sees Valhalla's raw answers on their way into Ferrostar, to keep the junction details
    private let junctionCapture = JunctionCapture()
    @ObservationIgnored private var pathAlong: Double?     // rider's last position along the route, metres
    @ObservationIgnored private var pathRouteCount = 0     // detects a new route (reroute) to reset pathAlong
    @ObservationIgnored private var tripStartedAt = Date()
    @ObservationIgnored private var tripKm = 0.0
    @ObservationIgnored private var podFeed: Task<Void, Never>?
    @ObservationIgnored private var routeRequestID = UUID()

    init(link: PodLink) {
        self.link = link
        device.startUpdating()   // also triggers the location permission prompt
        podFeed = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                self.updatePod()
                try? await Task.sleep(for: .seconds(1))
            }
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
        } else if let here = device.lastLocation {
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
            let router = try makeCore(device)
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
        } catch {
            guard routeRequestID == requestID else { return }
            phase = .failed("Routing failed: \(error.localizedDescription)")
        }
    }

    /// Step 2: start guidance on the previewed route.
    /// - Parameters:
    ///   - simulate: replay the route with fake GPS instead of riding it
    ///   - speedUp: simulation speed multiplier (1 = about 36 km/h)
    func startNavigation(simulate: Bool, speedUp: UInt64) {
        guard case let .preview(route) = phase else { return }
        let simulate = simulate || routeStartsElsewhere
        do {
            let core = try makeCore(simulate ? simulated : device)
            if simulate {
                simulated.warpFactor = max(1, speedUp)
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
            try core.startNavigation(route: route)
            tripStartedAt = Date()
            tripKm = route.distance / 1000
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
        isMuted.toggle()
        if let observer = core?.spokenInstructionObserver, observer.isMuted != isMuted {
            observer.toggleMute()
        }
    }

    func stop() {
        routeRequestID = UUID()
        stateWatch = nil
        core?.stopNavigation()
        core = nil
        navState = nil
        turnLine = ""
        tripLine = ""
        pathLine = ""
        poseLine = ""
        if phase != .idle { phase = .idle }
        updatePod()
    }

    private func handle(_ state: NavigationState?) {
        navState = state
        guard let state else { return }
        rerouting = state.isCalculatingNewRoute

        if case .complete = state.tripState {
            phase = .arrived
            updatePod()
            return
        }
        guard state.isNavigating,
              let progress = state.currentProgress,
              let content = state.currentVisualInstruction?.primaryContent
        else { return }

        instruction = content.text
        distanceToTurn = Int(progress.distanceToNextManeuver.rounded())
        remainingKm = progress.distanceRemaining / 1000

        let dir = Self.podDirection(type: content.maneuverType, modifier: content.maneuverModifier)
        turnLine = "\(dir.rawValue)|\(distanceToTurn)|\(Self.podText(Self.shortRoadName(content.text)))"

        let eta = Date().addingTimeInterval(progress.durationRemaining)
        tripLine = String(format: "TRIP|%@|%.1f", Self.clockFormat.string(from: eta), remainingKm)

        if state.routeGeometry.count != pathRouteCount {   // new route: forget the old position
            pathRouteCount = state.routeGeometry.count
            pathAlong = nil
        }
        let junctions = core?.route.map { junctionCapture.junctions(forRouteDistance: $0.distance) } ?? []
        if let rider = state.preferredUserLocation?.coordinates,
           let result = PathShape.around(rider: rider, route: state.routeGeometry,
                                         metresToTurn: progress.distanceToNextManeuver, previousAlong: pathAlong,
                                         junctions: junctions) {
            pathAlong = result.along
            // The pod draws the route on its map through a moving camera, so it wants the route
            // north-up around the same origin as POSE (turn the heading-up shape back)
            let h = result.headingDegrees * .pi / 180
            pathLine = "RPATH|" + result.shape.map { p -> String in
                let right = Double(p.x), ahead = Double(p.y)
                let east = right * cos(h) + ahead * sin(h), north = -right * sin(h) + ahead * cos(h)
                return "\(Int(east.rounded())),\(Int(north.rounded()))"
            }.joined(separator: ";")
            // Street map: same origin and heading as the route, so they line up
            let origin = result.origin
            poseLine = mapFeed.pose(lat: origin.lat, lng: origin.lng, headingDegrees: result.headingDegrees) ?? ""
            previousPoseLine = mapFeed.previousPose(lat: origin.lat, lng: origin.lng,
                                                    headingDegrees: result.headingDegrees) ?? ""
            if mapFeed.needsArea(lat: origin.lat, lng: origin.lng) {
                mapFeed.buildArea(lat: origin.lat, lng: origin.lng) { [weak self] lines in self?.sendMapArea(lines) }
            }
        }
        updatePod()
    }

    private static let clockFormat: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm"
        return f
    }()

    /// GPS speed for the pod's lean estimate, in tenths of a metre per second (simulated rides
    /// report the simulated speed)
    private var speedLine: String {
        let location = phase == .navigating ? navState?.preferredUserLocation : device.lastLocation
        let metresPerSecond = max(0, location?.speed?.value ?? 0)
        return "SPD|\(Int((metresPerSecond * 10).rounded()))"
    }

    /// What the pod should be showing right now, as (kind, line) pairs (see BLE_Link.h on the pod).
    private var podLines: [(kind: String, line: String)] {
        [("speed", speedLine)] + screenLines
    }

    private var screenLines: [(kind: String, line: String)] {
        switch phase {
        case .navigating:
            if rerouting { return [("state", "REROUTE")] + (tripLine.isEmpty ? [] : [("trip", tripLine)]) }
            guard !turnLine.isEmpty else { return [] }   // first instruction not computed yet
            // Turn first (the pod zooms from the turn distance), poses before the route (the pod
            // anchors the route to the latest pose)
            return [("turn", turnLine), ("pose", poseLine), ("pose_prev", previousPoseLine), ("path", pathLine),
                    ("trip", tripLine)]
                .filter { !$0.line.isEmpty }
        case .arrived:
            let minutes = Int((Date().timeIntervalSince(tripStartedAt) / 60).rounded())
            let place = Self.podText(destinationName)
            return [("state", String(format: "ARRIVED|%.1f|%d|", tripKm, minutes) + place)]
        case .idle, .routing, .preview, .failed:
            let metresPerSecond = max(0, device.lastLocation?.speed?.value ?? 0)
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
        for line in lines { link.send(line: line, priority: .bulk) }
        mapSentForConnection = link.connectionID
    }

    private func updatePod() {
        guard !podFeedPaused else { return }
        if phase == .navigating, !mapFeed.areaLines.isEmpty, mapSentForConnection != link.connectionID {
            sendMapArea(mapFeed.areaLines)          // the pod reconnected (or restarted): it has no map
        }
        let now = Date()
        for (kind, line) in podLines {
            let last = lastPodSent[kind]
            let age = now.timeIntervalSince(last?.at ?? .distantPast)
            let changed = line != last?.line
            let throttled = kind == "path" || kind == "stub" || kind.hasPrefix("pose") || kind == "speed"
            guard (changed && (!throttled || age >= 0.25)) || age >= 2 else { continue }
            lastPodSent[kind] = (line, now)
            link.send(line: line, kind: kind)
        }
    }

    /// The pod only draws left / right / straight arrows for now.
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

    /// The pod's fonts only contain plain ASCII characters
    private static func podText(_ text: String) -> String {
        String(String.UnicodeScalarView(text.unicodeScalars.filter { $0.isASCII && $0 != "|" }))
    }
}
