import CoreLocation
import FerrostarCoreFFI
import FerrostarMapLibreUI
import MapLibre

/// Where the arrow goes on the phone's navigation map, and which way it points.
///
/// Ferrostar snaps the rider to the current instruction's stretch of route and points the arrow
/// along the segment it found, which went wrong in two ways: at a vertex it sometimes took the
/// previous segment's direction, and at a turn the arrow stayed pinned to the corner, pointing
/// the old way, until the next instruction began. Here the GPS position is projected onto the
/// whole route and the arrow points along the route from 3 m behind to 10 m ahead: it follows
/// the road and turns smoothly through corners. Further than 30 m from the route, Ferrostar's own
/// placement is kept.
struct RoutePuck {
    private(set) var along: Double?          // metres along the route, to search near next time
    private var routeKey = ""                // a new route (reroute) starts the search afresh

    /// The arrow's location for this raw position, or nil to keep Ferrostar's
    mutating func place(_ raw: UserLocation, on route: [GeographicCoordinate]) -> CLLocation? {
        guard route.count >= 2 else { return nil }
        let key = "\(route.count)|\(route[0].lat)|\(route[0].lng)"
        if key != routeKey {
            routeKey = key
            along = nil
        }
        let lat0 = raw.coordinates.lat, lng0 = raw.coordinates.lng
        let mLat = 110_540.0, mLng = 111_320.0 * cos(lat0 * .pi / 180)
        let pts = route.map { (x: ($0.lng - lng0) * mLng, y: ($0.lat - lat0) * mLat) }   // rider at 0,0
        var cumulative = [0.0]
        for i in 1..<pts.count {
            cumulative.append(cumulative[i - 1] + hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y))
        }

        // Nearest point on the route; near the last position if known (a route can pass the same
        // place twice), else anywhere
        func nearest(window: Bool) -> (s: Double, d2: Double) {
            var best = (s: 0.0, d2: Double.infinity)
            for i in 0..<(pts.count - 1) {
                if window, let previous = along, cumulative[i + 1] < previous - 50 || cumulative[i] > previous + 300 { continue }
                let a = pts[i], b = pts[i + 1]
                let dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy
                let t = len2 > 0 ? max(0, min(1, -(a.x * dx + a.y * dy) / len2)) : 0
                let px = a.x + t * dx, py = a.y + t * dy
                let d2 = px * px + py * py
                if d2 < best.d2 { best = (cumulative[i] + t * sqrt(len2), d2) }
            }
            return best
        }
        var best = nearest(window: along != nil)
        if !best.d2.isFinite || best.d2 > 30 * 30 { best = nearest(window: false) }
        guard best.d2 <= 30 * 30 else {
            along = nil
            return nil
        }
        along = best.s

        let total = cumulative.last ?? 0
        func point(at s: Double) -> (x: Double, y: Double) {
            let s = min(max(s, 0), total)
            let i = min(max((cumulative.lastIndex { $0 <= s } ?? 0), 0), pts.count - 2)
            let length = cumulative[i + 1] - cumulative[i]
            let t = length > 0 ? (s - cumulative[i]) / length : 0
            return (pts[i].x + t * (pts[i + 1].x - pts[i].x), pts[i].y + t * (pts[i + 1].y - pts[i].y))
        }
        let here = point(at: best.s)
        let from = point(at: best.s - 3), to = point(at: best.s + 10)
        var course = atan2(to.x - from.x, to.y - from.y) * 180 / .pi   // clockwise from north
        if course < 0 { course += 360 }

        return CLLocation(
            coordinate: CLLocationCoordinate2D(latitude: lat0 + here.y / mLat, longitude: lng0 + here.x / mLng),
            altitude: 0, horizontalAccuracy: raw.horizontalAccuracy, verticalAccuracy: -1,
            course: course, courseAccuracy: 5,
            speed: raw.speed?.value ?? -1, speedAccuracy: raw.speed?.accuracy ?? -1,
            timestamp: raw.timestamp
        )
    }
}

/// The navigation map's location source: Ferrostar hands it the snapped location as usual, and it
/// passes on the navigator's route-following placement instead when there is one.
final class RoutePuckLocationManager: NSObject, NavigationDrivenLocationManager, @unchecked Sendable {
    var delegate: (any MLNLocationManagerDelegate)?
    var authorizationStatus: CLAuthorizationStatus = .authorizedAlways
    var headingOrientation: CLDeviceOrientation = .portrait
    /// The navigator's current placement (nil: use what Ferrostar sets)
    var placement: () -> CLLocation? = { nil }

    private var shown = CLLocation()

    var lastLocation: CLLocation {
        get { shown }
        set {
            let location = placement() ?? newValue
            // Ferrostar sets this on every view update; tell the map only when it moved or turned
            guard location.coordinate.latitude != shown.coordinate.latitude ||
                location.coordinate.longitude != shown.coordinate.longitude ||
                location.course != shown.course else { return }
            shown = location
            delegate?.locationManager(self, didUpdate: [location])
        }
    }

    var lastHeading: CLHeading? {
        didSet {
            guard let lastHeading else { return }
            delegate?.locationManager(self, didUpdate: lastHeading)
        }
    }

    func requestAlwaysAuthorization() {}
    func requestWhenInUseAuthorization() {}
    func startUpdatingLocation() {
        // As MapLibre's StaticLocationManager: dispatched, or a following camera doesn't update
        DispatchQueue.main.async { self.delegate?.locationManager(self, didUpdate: [self.shown]) }
    }
    func stopUpdatingLocation() {}
    func startUpdatingHeading() {}
    func stopUpdatingHeading() {}
    func dismissHeadingCalibrationDisplay() {}
}
