import CoreLocation
import FerrostarCore
import FerrostarCoreFFI
import Foundation

/// The phone's GPS, minus positions that would put the rider in the wrong place.
///
/// iOS hands out its last known position straight away, which can be minutes old, and the first
/// live fixes are often rough Wi-Fi / cell-tower guesses (100 m or worse). Ferrostar starts from
/// whatever it gets, so navigation began with the arrow somewhere else and jumped to the right
/// place a second later. Only recent, reasonably precise fixes pass through here.
final class FreshLocationProvider: NSObject, LocationProviding, ObservableObject {
    static let maxAge: TimeInterval = 10            // seconds
    static let maxAccuracyMetres = 65.0

    weak var delegate: LocationManagingDelegate?
    @Published private(set) var lastLocation: UserLocation?

    private let inner: CoreLocationProvider

    init(activityType: CLActivityType, allowBackgroundLocationUpdates: Bool) {
        inner = CoreLocationProvider(activityType: activityType,
                                     allowBackgroundLocationUpdates: allowBackgroundLocationUpdates)
        super.init()
        inner.delegate = self
        if let cached = inner.lastLocation, Self.isGood(cached) { lastLocation = cached }
    }

    var authorizationStatus: CLAuthorizationStatus { inner.authorizationStatus }
    var lastHeading: Heading? { inner.lastHeading }

    func startUpdating() { inner.startUpdating() }
    func stopUpdating() { inner.stopUpdating() }

    static func isGood(_ location: UserLocation) -> Bool {
        location.horizontalAccuracy >= 0 && location.horizontalAccuracy <= maxAccuracyMetres
            && -location.timestamp.timeIntervalSinceNow <= maxAge
    }
}

extension FreshLocationProvider: LocationManagingDelegate {
    func locationManager(_: LocationProviding, didUpdateLocations locations: [UserLocation]) {
        let good = locations.filter(Self.isGood)
        guard let latest = good.last else { return }
        lastLocation = latest
        delegate?.locationManager(self, didUpdateLocations: good)
    }

    func locationManager(_: LocationProviding, didUpdateHeading newHeading: Heading) {
        delegate?.locationManager(self, didUpdateHeading: newHeading)
    }

    func locationManager(_: LocationProviding, didFailWithError error: Error) {
        delegate?.locationManager(self, didFailWithError: error)
    }
}
