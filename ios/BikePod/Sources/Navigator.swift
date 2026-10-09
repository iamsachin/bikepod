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
    private(set) var instruction = ""
    private(set) var distanceToTurn = 0
    private(set) var remainingKm = 0.0
    private(set) var rerouting = false
    /// Full Ferrostar state, for the in-app navigation map
    private(set) var navState: NavigationState?
    private(set) var isMuted = false

    var hasGPS: Bool { device.lastLocation != nil }
    var currentCoordinate: CLLocationCoordinate2D? { device.lastLocation?.clLocation.coordinate }

    private let link: PodLink
    private let device = CoreLocationProvider(activityType: .automotiveNavigation,
                                              allowBackgroundLocationUpdates: true)
    private let simulated = SimulatedLocationProvider()
    private var core: FerrostarCore?
    @ObservationIgnored private var stateWatch: AnyCancellable?
    @ObservationIgnored private var lastPodMessage = ""
    @ObservationIgnored private var routeRequestID = UUID()

    init(link: PodLink) {
        self.link = link
        device.startUpdating()   // also triggers the location permission prompt
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

    private static func makeCore(_ provider: LocationProviding) throws -> FerrostarCore {
        try FerrostarCore(
            wellKnownRouteProvider: .valhalla(endpointUrl: valhallaURL, profile: "motorcycle"),
            locationProvider: provider,
            navigationControllerConfig: config
        )
    }

    /// Step 1: ask Valhalla for a motorcycle route from here and show it.
    func planRoute(to destination: CLLocationCoordinate2D, name: String) async {
        stop()
        guard let origin = device.lastLocation else {
            phase = .failed("No GPS position yet. Wait a few seconds and try again.")
            return
        }
        destinationName = name
        phase = .routing
        let requestID = UUID()
        routeRequestID = requestID
        do {
            let router = try Self.makeCore(device)
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
        do {
            let core = try Self.makeCore(simulate ? simulated : device)
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
        lastPodMessage = ""
        if phase != .idle { phase = .idle }
    }

    private func handle(_ state: NavigationState?) {
        navState = state
        guard let state else { return }
        rerouting = state.isCalculatingNewRoute

        if case .complete = state.tripState {
            phase = .arrived
            sendToPod(.straight, metres: 0, road: "Arrived")
            return
        }
        guard state.isNavigating,
              let progress = state.currentProgress,
              let content = state.currentVisualInstruction?.primaryContent
        else { return }

        instruction = content.text
        distanceToTurn = Int(progress.distanceToNextManeuver.rounded())
        remainingKm = progress.distanceRemaining / 1000

        let road = rerouting ? "Rerouting..." : content.text
        sendToPod(Self.podDirection(type: content.maneuverType, modifier: content.maneuverModifier),
                  metres: distanceToTurn, road: road)
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
        "Road": "Rd", "Street": "St", "Avenue": "Ave", "Main Road": "Main Rd",
        "Cross Road": "Cross Rd", "Highway": "Hwy", "Junction": "Jn", "Nagar": "Ngr",
    ]

    /// "Outer Ring Road; KC Reddy Ring Road" -> "Outer Ring Rd"
    static func shortRoadName(_ name: String) -> String {
        // OSM can list several names for one road, separated by ";": keep the first
        var short = name.split(separator: ";").first.map(String.init) ?? name
        for (long, abbr) in abbreviations {
            short = short.replacingOccurrences(of: "\\b\(long)\\b", with: abbr, options: .regularExpression)
        }
        return short.trimmingCharacters(in: .whitespaces)
    }

    private func sendToPod(_ dir: TurnDir, metres: Int, road: String) {
        // The pod's fonts only contain plain ASCII characters
        let asciiRoad = String(String.UnicodeScalarView(Self.shortRoadName(road).unicodeScalars.filter { $0.isASCII }))
        let message = "\(dir.rawValue)|\(metres)|\(asciiRoad)"
        guard message != lastPodMessage else { return }   // GPS updates often change nothing
        lastPodMessage = message
        link.send(dir, metres: metres, road: asciiRoad)
    }
}
