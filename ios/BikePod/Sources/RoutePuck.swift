import CoreLocation
import FerrostarCoreFFI
import FerrostarMapLibreUI
import MapLibre
import QuartzCore

/// Where the arrow goes on the phone's navigation map, and which way it points.
///
/// Ferrostar snaps the rider to the current instruction's stretch of route and points the arrow
/// along the segment it found, which went wrong at turns: at a vertex it sometimes took the
/// previous segment's direction, and the arrow stayed pinned to the corner pointing the old way
/// until the next instruction began. Here the GPS position is projected onto the whole route and
/// the arrow points along the route there (4 m behind to 4 m ahead, so it turns through a corner
/// the way the bike does). Further than 30 m from the route, Ferrostar's placement is kept.
///
/// The map is fed the arrow 15 times a second, sliding along the route between GPS fixes. Given
/// only the fixes, MapLibre glided its camera (which the arrow is drawn at) over a whole second
/// per fix but turned the arrow at once, so before a corner the arrow already pointed down the
/// new road while still drawn up to a second back on the old one.
struct RoutePuck {
    /// Where the rider is, in metres along the route (nil when off it)
    private(set) var along: Double?
    private var routeKey = ""                // a new route (reroute) starts afresh
    private var lat0 = 0.0, lng0 = 0.0
    private var mLat = 110_540.0, mLng = 111_320.0
    private var pts: [(x: Double, y: Double)] = []
    private var cumulative: [Double] = []

    /// Projects a GPS fix onto the route. Returns the distance along it, or nil if the rider is
    /// more than 30 m away; `newRoute` says the route changed since the last fix.
    mutating func place(_ raw: UserLocation, on route: [GeographicCoordinate]) -> (along: Double, newRoute: Bool)? {
        guard route.count >= 2 else { return nil }
        let key = "\(route.count)|\(route[0].lat)|\(route[0].lng)"
        let newRoute = key != routeKey
        if newRoute {
            routeKey = key
            along = nil
            // Flat projection in metres about the route's start (fine over a city ride)
            lat0 = route[0].lat
            lng0 = route[0].lng
            mLng = 111_320.0 * cos(lat0 * .pi / 180)
            pts = route.map { (x: ($0.lng - lng0) * mLng, y: ($0.lat - lat0) * mLat) }
            cumulative = [0]
            for i in 1..<pts.count {
                cumulative.append(cumulative[i - 1] + hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y))
            }
        }
        let rx = (raw.coordinates.lng - lng0) * mLng, ry = (raw.coordinates.lat - lat0) * mLat

        // Nearest point on the route; near the last position if known (a route can pass the same
        // place twice), else anywhere
        func nearest(window: Bool) -> (s: Double, d2: Double) {
            var best = (s: 0.0, d2: Double.infinity)
            for i in 0..<(pts.count - 1) {
                if window, let previous = along, cumulative[i + 1] < previous - 50 || cumulative[i] > previous + 300 { continue }
                let a = pts[i], b = pts[i + 1]
                let dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy
                let t = len2 > 0 ? max(0, min(1, ((rx - a.x) * dx + (ry - a.y) * dy) / len2)) : 0
                let px = a.x + t * dx - rx, py = a.y + t * dy - ry
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
        return (best.s, newRoute)
    }

    /// The arrow at `s` metres along the route: on it, pointing along it
    func location(at s: Double, horizontalAccuracy: Double, speed: Double) -> CLLocation? {
        guard pts.count >= 2, let total = cumulative.last else { return nil }
        func point(at s: Double) -> (x: Double, y: Double) {
            let s = min(max(s, 0), total)
            var lo = 0, hi = cumulative.count - 1                  // last vertex at or before s
            while hi - lo > 1 {
                let mid = (lo + hi) / 2
                if cumulative[mid] <= s { lo = mid } else { hi = mid }
            }
            let length = cumulative[lo + 1] - cumulative[lo]
            let t = length > 0 ? (s - cumulative[lo]) / length : 0
            return (pts[lo].x + t * (pts[lo + 1].x - pts[lo].x), pts[lo].y + t * (pts[lo + 1].y - pts[lo].y))
        }
        let here = point(at: s)
        let from = point(at: s - 4), to = point(at: s + 4)
        var course = atan2(to.x - from.x, to.y - from.y) * 180 / .pi   // clockwise from north
        if course < 0 { course += 360 }
        return CLLocation(
            coordinate: CLLocationCoordinate2D(latitude: lat0 + here.y / mLat, longitude: lng0 + here.x / mLng),
            altitude: 0, horizontalAccuracy: horizontalAccuracy, verticalAccuracy: -1,
            course: course, courseAccuracy: 5, speed: speed, speedAccuracy: -1, timestamp: Date()
        )
    }
}

/// The arrow sliding along the route from where it was shown to the latest fix, over about the
/// time between fixes (so it arrives as the next one comes)
struct PuckMotion {
    var from: Double
    var to: Double
    var start: CFTimeInterval
    var duration: CFTimeInterval
    var horizontalAccuracy: Double
    var speed: Double

    func along(at time: CFTimeInterval) -> Double {
        guard duration > 0 else { return to }
        let t = min(max((time - start) / duration, 0), 1)
        return from + (to - from) * t
    }
}

/// The navigation map's location source. While the navigator has an arrow on the route, a timer
/// sends it 15 times a second; otherwise Ferrostar's own locations pass through.
final class RoutePuckLocationManager: NSObject, NavigationDrivenLocationManager, @unchecked Sendable {
    var delegate: (any MLNLocationManagerDelegate)?
    var authorizationStatus: CLAuthorizationStatus = .authorizedAlways
    var headingOrientation: CLDeviceOrientation = .portrait
    /// The navigator's arrow at a given time (nil: use what Ferrostar sets)
    var placement: (CFTimeInterval) -> CLLocation? = { _ in nil }

    private var shown = CLLocation()
    private var timer: Timer?

    override init() {
        super.init()
        let timer = Timer(timeInterval: 1.0 / 15, repeats: true) { [weak self] _ in self?.tick() }
        RunLoop.main.add(timer, forMode: .common)
        self.timer = timer
    }

    deinit { timer?.invalidate() }

    private func tick() {
        if let location = placement(CACurrentMediaTime()) { deliver(location) }
    }

    private func deliver(_ location: CLLocation) {
        guard location.coordinate.latitude != shown.coordinate.latitude ||
            location.coordinate.longitude != shown.coordinate.longitude ||
            location.course != shown.course else { return }
        shown = location
        delegate?.locationManager(self, didUpdate: [location])
    }

    var lastLocation: CLLocation {
        get { shown }
        // Ferrostar sets this on every view update: used only when there is no arrow on the route
        set { if placement(CACurrentMediaTime()) == nil { deliver(newValue) } }
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
