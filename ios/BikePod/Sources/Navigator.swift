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
        case navigating
        case arrived
        case failed(String)
    }

    private(set) var phase: Phase = .idle
    private(set) var instruction = ""
    private(set) var distanceToTurn = 0
    private(set) var remainingKm = 0.0
    private(set) var rerouting = false

    var hasGPS: Bool { device.lastLocation != nil }

    private let link: PodLink
    private let device = CoreLocationProvider(activityType: .automotiveNavigation,
                                              allowBackgroundLocationUpdates: true)
    private let simulated = SimulatedLocationProvider()
    private var core: FerrostarCore?
    @ObservationIgnored private var stateWatch: AnyCancellable?
    @ObservationIgnored private var lastPodMessage = ""

    init(link: PodLink) {
        self.link = link
        device.startUpdating()   // also triggers the location permission prompt
    }

    /// - Parameters:
    ///   - simulate: replay the route with fake GPS instead of riding it
    ///   - speedUp: simulation speed multiplier (1 = about 36 km/h)
    func start(to destination: CLLocationCoordinate2D, simulate: Bool, speedUp: UInt64) async {
        stop()
        guard let origin = device.lastLocation else {
            phase = .failed("No GPS position yet. Wait a few seconds and try again.")
            return
        }
        phase = .routing

        let provider: LocationProviding = simulate ? simulated : device
        let config = SwiftNavigationControllerConfig(
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

        do {
            let core = try FerrostarCore(
                wellKnownRouteProvider: .valhalla(endpointUrl: Self.valhallaURL, profile: "motorcycle"),
                locationProvider: provider,
                navigationControllerConfig: config
            )
            self.core = core
            let routes = try await core.getRoutes(
                initialLocation: origin,
                waypoints: [Waypoint(coordinate: GeographicCoordinate(lat: destination.latitude,
                                                                       lng: destination.longitude),
                                     kind: .break)]
            )
            guard self.core === core else { return }   // stopped while the route was loading
            guard let route = routes.first else {
                phase = .failed("No route found to that destination.")
                return
            }
            if simulate {
                simulated.warpFactor = max(1, speedUp)
                // One simulated GPS point every 10 m, once per second (divided by the speed-up)
                try simulated.setSimulatedRoute(route, resampleDistance: 10)
            }
            stateWatch = core.$state
                .receive(on: DispatchQueue.main)
                .sink { [weak self] state in self?.handle(state) }
            try core.startNavigation(route: route)
            phase = .navigating
        } catch {
            phase = .failed("Routing failed: \(error.localizedDescription)")
        }
    }

    func stop() {
        stateWatch = nil
        core?.stopNavigation()
        core = nil
        lastPodMessage = ""
        if phase != .idle { phase = .idle }
    }

    private func handle(_ state: NavigationState?) {
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
