import FerrostarCoreFFI
import Foundation

/// Things along the route from OpenStreetMap, fetched once per route from the public Overpass
/// API: speed breakers (`traffic_calming` nodes) and fuel stations (`amenity=fuel`). Not
/// everything is mapped: a warning is a bonus, not a guarantee.
@MainActor
final class Hazards {
    struct Kind {
        let name: String            // logs and the cache file
        let query: String           // Overpass statement before the box, e.g. node[traffic_calming]
        let keep: ([String: String]?) -> Bool
        let onRouteMetres: Double   // this close to the route line counts as on it
        let mergeMetres: Double     // closer together than this counts as one

        /// Breakers worth a warning on a motorcycle (islands, chicanes and the like are not)
        static let breakers = Kind(name: "breakers", query: "node[traffic_calming]", keep: { tags in
            ["bump", "hump", "table", "cushion", "mini_bumps", "rumble_strip", "dip"].contains(tags?["traffic_calming"] ?? "")
        }, onRouteMetres: 12, mergeMetres: 25)
        /// Fuel stations (often mapped as an area: its centre): beside the road, so further out
        static let fuel = Kind(name: "fuel", query: "nwr[amenity=fuel]", keep: { _ in true },
                               onRouteMetres: 80, mergeMetres: 60)
    }

    let kind: Kind

    init(_ kind: Kind = .breakers) {
        self.kind = kind
    }

    /// Distances along the route (metres from its start) of the breakers on it, ascending
    private(set) var along: [Double] = []
    private var routeKey = 0
    private var task: Task<Void, Never>?

    /// Fetches the breakers for this route (once per route: reroutes give a new one)
    func load(route: [GeographicCoordinate]) {
        let key = route.count &* 31 &+ Int((route.first?.lat ?? 0) * 1e6) &+ Int((route.last?.lng ?? 0) * 1e6)
        guard key != routeKey, route.count >= 2 else { return }
        routeKey = key
        along = []
        task?.cancel()
        task = Task { [weak self] in
            // The public servers are often busy: keep trying for a while during the ride
            for wait in [0, 20, 60, 120, 300] {
                if wait > 0 { try? await Task.sleep(for: .seconds(wait)) }
                guard !Task.isCancelled else { return }
                guard let kind = self?.kind else { return }
                if let nodes = await Self.points(kind, around: route) {
                    let found = Self.project(nodes, onto: route, kind: kind)
                    DebugLog.write("HAZARDS \(nodes.count) \(kind.name) near the route, \(found.count) on it")
                    self?.along = found
                    return
                }
            }
        }
    }

    func clear() {
        task?.cancel()
        routeKey = 0
        along = []
    }

    /// Metres to the next breaker ahead of `position` (metres along the route), if any
    func next(after position: Double) -> Double? {
        along.first { $0 > position + 3 }.map { $0 - position }
    }

    /// The point `s` metres along the route (the same flat projection as the breakers' distances)
    static func coordinate(on route: [GeographicCoordinate], atAlong s: Double) -> (lat: Double, lng: Double)? {
        guard route.count >= 2 else { return nil }
        let ref = route[0]
        let mLat = 110_540.0, mLng = 111_320.0 * cos(ref.lat * .pi / 180)
        var travelled = 0.0
        for i in 0..<(route.count - 1) {
            let a = route[i], b = route[i + 1]
            let length = hypot((b.lng - a.lng) * mLng, (b.lat - a.lat) * mLat)
            if travelled + length >= s {
                let t = length > 0 ? (s - travelled) / length : 0
                return (a.lat + t * (b.lat - a.lat), a.lng + t * (b.lng - a.lng))
            }
            travelled += length
        }
        return nil
    }

    /// Breakers around the route: from the disk cache when an earlier fetch covered this box,
    /// otherwise from Overpass (and then remembered)
    private static func points(_ kind: Kind, around route: [GeographicCoordinate]) async -> [(lat: Double, lng: Double)]? {
        let pad = kind.onRouteMetres / 100_000 + 0.0004           // a bit beyond what counts as on it
        let box = Box(s: (route.map(\.lat).min() ?? 0) - pad, w: (route.map(\.lng).min() ?? 0) - pad,
                      n: (route.map(\.lat).max() ?? 0) + pad, e: (route.map(\.lng).max() ?? 0) + pad)
        var cache = Cache.load(kind.name)
        if cache.covered.contains(where: { $0.contains(box) }) {
            return cache.points.filter { box.contains($0.lat, $0.lng) }.map { ($0.lat, $0.lng) }
        }
        guard let fresh = await fetch(box, kind: kind) else { return nil }
        cache.covered.append(box)
        let known = Set(cache.points.map { "\($0.lat),\($0.lng)" })
        cache.points += fresh.filter { !known.contains("\($0.lat),\($0.lng)") }.map { Cache.Point(lat: $0.lat, lng: $0.lng) }
        cache.save(kind.name)
        return fresh
    }

    struct Box: Codable {
        let s, w, n, e: Double
        func contains(_ other: Box) -> Bool { other.s >= s && other.n <= n && other.w >= w && other.e <= e }
        func contains(_ lat: Double, _ lng: Double) -> Bool { lat >= s && lat <= n && lng >= w && lng <= e }
    }

    /// Every breaker fetched so far and the boxes the fetches covered (OSM changes slowly; the
    /// cache is dropped after 30 days so new breakers get picked up)
    private struct Cache: Codable {
        struct Point: Codable { let lat: Double; let lng: Double }
        var covered: [Box] = []
        var points: [Point] = []
        var created = Date()

        static func url(_ name: String) -> URL {
            FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("\(name).json")
        }

        static func load(_ name: String) -> Cache {
            guard let data = try? Data(contentsOf: url(name)), let cache = try? JSONDecoder().decode(Cache.self, from: data),
                  Date().timeIntervalSince(cache.created) < 30 * 86_400
            else { return Cache() }
            return cache
        }

        func save(_ name: String) {
            if let data = try? JSONEncoder().encode(self) { try? data.write(to: Self.url(name)) }
        }
    }

    private static func fetch(_ box: Box, kind: Kind) async -> [(lat: Double, lng: Double)]? {
        let (s, w, n, e) = (box.s, box.w, box.n, box.e)
        let query = "[out:json][timeout:25];\(kind.query)(\(s),\(w),\(n),\(e));out center;"
        struct Response: Decodable {
            struct Centre: Decodable { let lat: Double; let lon: Double }
            struct Element: Decodable { let lat: Double?; let lon: Double?; let center: Centre?; let tags: [String: String]? }
            let elements: [Element]
        }
        // The public servers sometimes answer "too busy" (an error page, not JSON): retry, then a mirror
        for (attempt, server) in servers.enumerated() {
            if attempt > 0 { try? await Task.sleep(for: .seconds(2)) }
            guard !Task.isCancelled else { return nil }
            var request = URLRequest(url: URL(string: server)!)
            request.httpMethod = "POST"
            request.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
            request.httpBody = "data=\(query.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? "")".data(using: .utf8)
            request.setValue("BikePod/0.1 (personal project)", forHTTPHeaderField: "User-Agent")
            request.timeoutInterval = 30
            do {
                let (data, response) = try await URLSession.shared.data(for: request)
                let status = (response as? HTTPURLResponse)?.statusCode ?? 0
                guard status == 200, let decoded = try? JSONDecoder().decode(Response.self, from: data) else {
                    DebugLog.write("HAZARDS \(server) answered \(status): \(String(decoding: data.prefix(80), as: UTF8.self))")
                    continue
                }
                return decoded.elements
                    .filter { kind.keep($0.tags) }
                    .compactMap { e in
                        if let lat = e.lat, let lon = e.lon { return (lat, lon) }      // a node
                        return e.center.map { ($0.lat, $0.lon) }                       // an area's centre
                    }
            } catch {
                DebugLog.write("HAZARDS \(server) failed: \(error.localizedDescription)")
            }
        }
        return nil
    }

    private static let servers = [
        "https://overpass-api.de/api/interpreter",
        "https://overpass-api.de/api/interpreter",
        "https://overpass.private.coffee/api/interpreter",
    ]

    /// Along-route distances of the points within `onRouteMetres` of the route (same flat
    /// projection as PathShape, so the distances match its `along`)
    private static func project(_ points: [(lat: Double, lng: Double)], onto route: [GeographicCoordinate],
                                kind: Kind) -> [Double] {
        let ref = route[0]
        let mLat = 110_540.0, mLng = 111_320.0 * cos(ref.lat * .pi / 180)
        let pts = route.map { (x: ($0.lng - ref.lng) * mLng, y: ($0.lat - ref.lat) * mLat) }
        var cumulative = [0.0]
        for i in 1..<pts.count { cumulative.append(cumulative[i - 1] + hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y)) }

        var result: [Double] = []
        for p in points {
            let q = (x: (p.lng - ref.lng) * mLng, y: (p.lat - ref.lat) * mLat)
            var best = (d2: Double.infinity, s: 0.0)
            for i in 0..<(pts.count - 1) {
                let a = pts[i], b = pts[i + 1], dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy
                let t = len2 > 0 ? max(0, min(1, ((q.x - a.x) * dx + (q.y - a.y) * dy) / len2)) : 0
                let ex = a.x + t * dx - q.x, ey = a.y + t * dy - q.y
                let d2 = ex * ex + ey * ey
                if d2 < best.d2 { best = (d2, cumulative[i] + t * sqrt(len2)) }
            }
            if best.d2 <= kind.onRouteMetres * kind.onRouteMetres { result.append(best.s) }
        }
        // Breakers in a row (pairs either side of a crossing, rumble strips) count as one warning
        var merged: [Double] = []
        for s in result.sorted() where merged.last.map({ s - $0 > kind.mergeMetres }) ?? true { merged.append(s) }
        return merged
    }
}
