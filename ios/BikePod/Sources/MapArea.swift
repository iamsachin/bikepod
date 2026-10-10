import Foundation

/// Turns map-tile roads into the pod's MAP lines for one "area" around a reference point,
/// and the rider's position into POSE lines in that area's frame (see firmware/pod/Map_Layer.h).
///
/// Coordinates are whole metres, north-up: +x east, +y north of the reference point.
enum MapArea {
    static let radiusMetres = 500.0
    static let refreshMetres = 120.0       // build a new area after riding this far from the last one
    static let maxPoints = 2200            // the pod keeps up to 2400 points per area
    static let pointsPerLine = 18          // one MAP line stays well under a BLE packet / serial line

    struct Frame {
        let refLat: Double
        let refLng: Double
        var metresPerDegLat: Double { 110_540 }
        var metresPerDegLng: Double { 111_320 * cos(refLat * .pi / 180) }

        func project(_ lat: Double, _ lng: Double) -> (x: Double, y: Double) {
            ((lng - refLng) * metresPerDegLng, (lat - refLat) * metresPerDegLat)
        }
    }

    static let maxFuel = 48                // the pod keeps up to 48 fuel stations per area

    /// MAPBEGIN, one MAP line per road piece, FUEL lines (fuel stations), MAPEND
    static func lines(id: Int, frame: Frame, roads: [MapRoad], fuel: [(lat: Double, lng: Double)] = []) -> [String] {
        // Keep the parts of each road inside the area, simplified to ~2.5 m
        var pieces: [(roadClass: RoadClass, points: [(x: Int, y: Int)], nearest: Double)] = []
        let limit = radiusMetres * 1.15
        for road in roads {
            var run: [(x: Double, y: Double)] = []
            func flush() {
                if run.count >= 2 {
                    let simple = simplify(run, 2.5).map { (x: Int($0.x.rounded()), y: Int($0.y.rounded())) }
                    let nearest = run.map { hypot($0.x, $0.y) }.min() ?? 0
                    pieces.append((road.roadClass, simple, nearest))
                }
                run = []
            }
            for p in road.points {
                let m = frame.project(p.lat, p.lng)
                if hypot(m.x, m.y) <= limit { run.append(m) } else { flush() }
            }
            flush()
        }

        // Too much map (very dense areas): keep big roads first, then the nearest
        pieces.sort { ($0.roadClass.rawValue, $0.nearest) < ($1.roadClass.rawValue, $1.nearest) }
        var budget = maxPoints
        var out = ["MAPBEGIN|\(id)"]
        var roadCount = 0
        for piece in pieces {
            var start = 0
            while start < piece.points.count - 1 {
                let end = min(start + pointsPerLine, piece.points.count)
                let chunk = piece.points[start..<end]
                guard budget >= chunk.count else { break }
                budget -= chunk.count
                out.append("MAP|\(id)|\(piece.roadClass.rawValue)|" + chunk.map { "\($0.x),\($0.y)" }.joined(separator: ";"))
                roadCount += 1
                start = end - 1                        // chunks share their joining point
            }
        }
        // Fuel stations in the area, nearest first, a few per line. Tiles can list a station twice
        // (e.g. as a point and as the label of its forecourt): merge any within 15 m.
        var unique: [(x: Double, y: Double)] = []
        for p in fuel.map({ frame.project($0.lat, $0.lng) }).filter({ hypot($0.x, $0.y) <= limit })
        where !unique.contains(where: { hypot($0.x - p.x, $0.y - p.y) < 15 }) {
            unique.append(p)
        }
        let pumps = unique
            .sorted { hypot($0.x, $0.y) < hypot($1.x, $1.y) }
            .prefix(maxFuel)
            .map { "\(Int($0.x.rounded())),\(Int($0.y.rounded()))" }
        for start in stride(from: 0, to: pumps.count, by: 16) {
            out.append("FUEL|\(id)|" + pumps[start..<min(start + 16, pumps.count)].joined(separator: ";"))
        }
        out.append("MAPEND|\(id)|\(roadCount)")
        return out
    }

    static func pose(id: Int, frame: Frame, lat: Double, lng: Double, headingDegrees: Double) -> String {
        let p = frame.project(lat, lng)
        var h = headingDegrees.truncatingRemainder(dividingBy: 360)
        if h < 0 { h += 360 }
        return "POSE|\(id)|\(Int(p.x.rounded())),\(Int(p.y.rounded())),\(Int(h.rounded()) % 360)"
    }

    /// Douglas-Peucker on metre points
    private static func simplify(_ line: [(x: Double, y: Double)], _ tolerance: Double) -> [(x: Double, y: Double)] {
        guard line.count > 2 else { return line }
        let a = line[0], b = line[line.count - 1]
        let dx = b.x - a.x, dy = b.y - a.y, len = hypot(dx, dy)
        var worst = 0.0, index = 0
        for i in 1..<(line.count - 1) {
            let p = line[i]
            let d = len > 0 ? abs((p.x - a.x) * dy - (p.y - a.y) * dx) / len : hypot(p.x - a.x, p.y - a.y)
            if d > worst { worst = d; index = i }
        }
        guard worst > tolerance else { return [a, b] }
        return simplify(Array(line[...index]), tolerance) + simplify(Array(line[index...]), tolerance).dropFirst()
    }
}
