import FerrostarCoreFFI
import Foundation

/// The stretch of route around the rider, for the pod's route drawing.
///
/// Points are in metres relative to the rider, rotated heading-up:
/// +x is to the right, +y is straight ahead, and the rider is exactly (0, 0).
/// The route itself sets the heading (not the GPS course), so it stays steady at low speed.
enum PathShape {
    static let behindMetres = 30.0
    static let beyondTurnMetres = 200.0   // long enough to reach the edge of the screen
    static let maxAheadMetres = 700.0
    static let maxPoints = 20          // keeps one PATH line within a single BLE packet
    static let maxStubs = 14           // keeps one STUB line within a single BLE packet

    /// A side street to draw: where it leaves the route (metres, same frame as the shape),
    /// its direction in degrees clockwise from straight ahead, and whether it is at the turn
    /// (drawn long, across the screen, like the road being turned onto).
    typealias Stub = (x: Int, y: Int, angle: Int, atTurn: Bool)

    /// - Parameter previousAlong: where the rider was along the route last time (metres from the
    ///   start). Routes can double back on themselves (U-turns on divided roads), so the nearest
    ///   piece of route may be the other carriageway; searching only near the last position avoids that.
    /// - Parameter junctions: Valhalla's junctions for this route (see JunctionCapture), for side streets.
    /// - Returns: the shape, side-street stubs, and the rider's position along the route to pass in next time.
    static func around(rider: GeographicCoordinate, route: [GeographicCoordinate],
                       metresToTurn: Double, previousAlong: Double? = nil, junctions: [Junction] = [])
        -> (shape: [(x: Int, y: Int)], stubs: [Stub], along: Double,
            origin: (lat: Double, lng: Double), headingDegrees: Double)? {
        guard route.count >= 2 else { return nil }

        // Flat local projection in metres around the rider (fine over a few hundred metres)
        let metresPerDegLat = 110_540.0
        let metresPerDegLng = 111_320.0 * cos(rider.lat * .pi / 180)
        let pts = route.map { Point(x: ($0.lng - rider.lng) * metresPerDegLng,
                                    y: ($0.lat - rider.lat) * metresPerDegLat) }

        // Distance along the route to every vertex
        var along = [0.0]
        for i in 1..<pts.count { along.append(along[i - 1] + pts[i - 1].distance(to: pts[i])) }

        // Where the rider is along the route: nearest point on a segment (near the last position if known)
        var best = (distanceSq: Double.infinity, s: 0.0)
        for i in 0..<(pts.count - 1) {
            if let previous = previousAlong, along[i + 1] < previous - 50 || along[i] > previous + 400 { continue }
            let (t, d2) = Point.zero.projection(onto: pts[i], pts[i + 1])
            if d2 < best.distanceSq { best = (d2, along[i] + t * pts[i].distance(to: pts[i + 1])) }
        }
        guard best.distanceSq.isFinite else {
            // Jumped (e.g. after a long GPS gap): search the whole route
            return around(rider: rider, route: route, metresToTurn: metresToTurn, junctions: junctions)
        }
        let s0 = best.s
        let total = along.last ?? 0

        func point(at s: Double) -> Point {
            let s = min(max(s, 0), total)
            guard let i = along.lastIndex(where: { $0 <= s }), i < pts.count - 1 else { return pts.last! }
            let length = along[i + 1] - along[i]
            let t = length > 0 ? (s - along[i]) / length : 0
            return pts[i].lerp(to: pts[i + 1], t)
        }

        // Heading from the route just behind to a little ahead of the rider
        let from = point(at: s0 - 5), to = point(at: s0 + 25)
        let heading = atan2(to.x - from.x, to.y - from.y)   // radians clockwise from north
        let origin = point(at: s0)
        func local(_ p: Point) -> Point {
            let dx = p.x - origin.x, dy = p.y - origin.y
            return Point(x: dx * cos(heading) - dy * sin(heading),     // right
                         y: dx * sin(heading) + dy * cos(heading))     // ahead
        }

        let start = s0 - behindMetres
        let end = min(s0 + min(metresToTurn + beyondTurnMetres, maxAheadMetres), total)
        var behind = [local(point(at: start))]
        var ahead = [Point.zero]
        for (i, s) in along.enumerated() where s > start && s < end {
            if s < s0 { behind.append(local(pts[i])) } else if s > s0 { ahead.append(local(pts[i])) }
        }
        behind.append(.zero)
        ahead.append(local(point(at: end)))

        // Simplify until it fits, never dropping the rider's own point
        var tolerance = 2.0
        var shape: [Point]
        repeat {
            shape = simplify(behind, tolerance) + simplify(ahead, tolerance).dropFirst()
            tolerance *= 1.5
        } while shape.count > maxPoints

        // Round to whole metres, dropping repeats (but keeping the rider at exactly 0,0)
        var result: [(x: Int, y: Int)] = []
        for p in shape {
            let q = p == .zero ? (x: 0, y: 0) : (x: Int(p.x.rounded()), y: Int(p.y.rounded()))
            if let last = result.last, last == q { continue }
            result.append(q)
        }
        // Side streets at the junctions in view, nearest first
        let headingDegrees = heading * 180 / .pi
        var stubs: [(distance: Double, stub: Stub)] = []
        for junction in junctions where junction.geometryIndex < pts.count {
            let s = along[junction.geometryIndex]
            guard s >= start, s <= end else { continue }
            let p = local(pts[junction.geometryIndex])
            let atTurn = abs(s - (s0 + metresToTurn)) < 15
            for bearing in junction.sideBearings {
                let angle = (bearing - headingDegrees).truncatingRemainder(dividingBy: 360)
                stubs.append((atTurn ? -1 : abs(s - s0),     // the turn's streets first
                              (Int(p.x.rounded()), Int(p.y.rounded()),
                               Int((angle < 0 ? angle + 360 : angle).rounded()) % 360, atTurn)))
            }
        }
        stubs.sort { $0.distance < $1.distance }
        // Where the drawing's (0,0) is on Earth, and its heading: the map uses the same view
        let originLatLng = (lat: rider.lat + origin.y / metresPerDegLat, lng: rider.lng + origin.x / metresPerDegLng)
        return (result, stubs.prefix(maxStubs).map(\.stub), s0, originLatLng, headingDegrees)
    }

    /// Douglas-Peucker line simplification; keeps both end points.
    private static func simplify(_ line: [Point], _ tolerance: Double) -> [Point] {
        guard line.count > 2 else { return line }
        var maxDistance = 0.0, index = 0
        for i in 1..<(line.count - 1) {
            let d = line[i].distanceToLine(line[0], line[line.count - 1])
            if d > maxDistance { maxDistance = d; index = i }
        }
        guard maxDistance > tolerance else { return [line[0], line[line.count - 1]] }
        let left = simplify(Array(line[...index]), tolerance)
        let right = simplify(Array(line[index...]), tolerance)
        return left + right.dropFirst()
    }
}

private struct Point: Equatable {
    var x: Double
    var y: Double
    static let zero = Point(x: 0, y: 0)

    func distance(to p: Point) -> Double { hypot(p.x - x, p.y - y) }

    func lerp(to p: Point, _ t: Double) -> Point { Point(x: x + (p.x - x) * t, y: y + (p.y - y) * t) }

    /// Fraction along a→b of the closest point to self, and the squared distance to it
    func projection(onto a: Point, _ b: Point) -> (t: Double, distanceSq: Double) {
        let dx = b.x - a.x, dy = b.y - a.y
        let lengthSq = dx * dx + dy * dy
        let t = lengthSq > 0 ? min(max(((x - a.x) * dx + (y - a.y) * dy) / lengthSq, 0), 1) : 0
        let px = a.x + t * dx - x, py = a.y + t * dy - y
        return (t, px * px + py * py)
    }

    func distanceToLine(_ a: Point, _ b: Point) -> Double {
        sqrt(projection(onto: a, b).distanceSq)
    }
}
