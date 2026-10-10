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
        if let cached = inner.lastLocation {
            DebugLog.write(String(format: "CACHED %.6f,%.6f acc %.0f m age %.0f s %@", cached.coordinates.lat, cached.coordinates.lng,
                                  cached.horizontalAccuracy, -cached.timestamp.timeIntervalSinceNow, Self.isGood(cached) ? "ok" : "REJECTED"))
            if Self.isGood(cached) { lastLocation = cached }
        }
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
        for l in locations where !Self.isGood(l) {            // only the ones held back
            DebugLog.write(String(format: "FIX %.6f,%.6f acc %.0f m age %.1f s %@", l.coordinates.lat, l.coordinates.lng,
                                  l.horizontalAccuracy, -l.timestamp.timeIntervalSinceNow, Self.isGood(l) ? "ok" : "REJECTED"))
        }
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
