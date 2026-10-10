import Foundation

/// Keeps the pod supplied with the streets around the rider.
///
/// Downloads OpenFreeMap vector tiles (cached in memory and on disk), and every
/// `MapArea.refreshMetres` of riding builds a new area of roads around the rider. The lines go to
/// the pod once per area; between areas only the rider's pose is sent (see MapArea.pose).
@MainActor
final class MapFeed {
    private(set) var areaID = 0
    private(set) var frame: MapArea.Frame?
    private(set) var areaLines: [String] = []
    private var building = false
    /// When building an area last failed (tiles couldn't be fetched): retried after a pause
    private var failedAt: Date?
    private var tileTemplate: String?
    private var decoded: [String: TileContent] = [:]     // tile URL -> roads and fuel stations, in memory

    private struct TileContent {
        let roads: [MapRoad]
        let fuel: [(lat: Double, lng: Double)]
    }

    private static let zoom = 14
    private static let fallbackTemplate = "https://tiles.openfreemap.org/planet/20261004_113936_pt/{z}/{x}/{y}.pbf"
    private static let cacheDir: URL = {
        let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("tiles")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }()

    /// True when the rider has moved far enough from the current area for a new one
    func needsArea(lat: Double, lng: Double) -> Bool {
        if let failedAt, Date().timeIntervalSince(failedAt) < 10 { return false }
        guard let frame else { return true }
        let p = frame.project(lat, lng)
        return hypot(p.x, p.y) > MapArea.refreshMetres
    }

    /// Builds a new area around (lat, lng) in the background; `done` gets the area's lines.
    func buildArea(lat: Double, lng: Double, done: @escaping ([String]) -> Void) {
        guard !building else { return }
        building = true
        Task {
            defer { building = false }
            let template = await resolveTemplate()
            var roads: [MapRoad] = []
            var fuel: [(lat: Double, lng: Double)] = []
            for t in VectorTile.tiles(around: lat, lng, radiusMetres: MapArea.radiusMetres, z: Self.zoom) {
                // A missing tile (no signal) would send the pod an area with a hole or no roads at
                // all, replacing a good one: keep the current area and try again shortly
                guard let content = await tileContent(template: template, x: t.x, y: t.y) else {
                    failedAt = Date()
                    return
                }
                roads += content.roads
                fuel += content.fuel
            }
            failedAt = nil
            let nextID = areaID % 60_000 + 1
            let newFrame = MapArea.Frame(refLat: lat, refLng: lng)
            let lines = await Task.detached(priority: .utility) {
                MapArea.lines(id: nextID, frame: newFrame, roads: roads, fuel: fuel)
            }.value
            areaID = nextID
            frame = newFrame
            areaLines = lines
            done(lines)
        }
    }

    func pose(lat: Double, lng: Double, headingDegrees: Double) -> String? {
        guard let frame else { return nil }
        return MapArea.pose(id: areaID, frame: frame, lat: lat, lng: lng, headingDegrees: headingDegrees)
    }

    /// Downloads the tiles around a point ahead of time (e.g. the start of a planned route)
    func prefetch(lat: Double, lng: Double) {
        Task {
            let template = await resolveTemplate()
            for t in VectorTile.tiles(around: lat, lng, radiusMetres: MapArea.radiusMetres, z: Self.zoom) {
                _ = await tileContent(template: template, x: t.x, y: t.y)
            }
        }
    }

    /// The tile URL pattern changes when OpenFreeMap publishes new data: read it from their TileJSON
    private func resolveTemplate() async -> String {
        if let tileTemplate { return tileTemplate }
        struct TileJSON: Decodable { let tiles: [String] }
        if let url = URL(string: "https://tiles.openfreemap.org/planet"),
           let (data, _) = try? await URLSession.shared.data(from: url),
           let json = try? JSONDecoder().decode(TileJSON.self, from: data), let first = json.tiles.first {
            tileTemplate = first
        }
        return tileTemplate ?? Self.fallbackTemplate
    }

    /// A tile's roads and fuel stations; nil if it couldn't be fetched
    private func tileContent(template: String, x: Int, y: Int) async -> TileContent? {
        let urlString = template.replacingOccurrences(of: "{z}", with: "\(Self.zoom)")
            .replacingOccurrences(of: "{x}", with: "\(x)").replacingOccurrences(of: "{y}", with: "\(y)")
        if let content = decoded[urlString] { return content }
        guard let url = URL(string: urlString) else { return nil }

        // Disk cache, named after the URL path (it includes the data version, so new data is refetched)
        let name = url.path.split(separator: "/").joined(separator: "_")
        let file = Self.cacheDir.appendingPathComponent(name)
        var data = try? Data(contentsOf: file)
        if data == nil, let (fresh, response) = try? await URLSession.shared.data(from: url),
           (response as? HTTPURLResponse)?.statusCode == 200 {
            data = fresh
            try? fresh.write(to: file)
        }
        guard let data else { return nil }
        let content = await Task.detached(priority: .utility) {
            TileContent(roads: VectorTile.roads(from: data, z: Self.zoom, x: x, y: y),
                        fuel: VectorTile.fuelStations(from: data, z: Self.zoom, x: x, y: y))
        }.value
        if decoded.count > 16 { decoded.removeAll() }
        decoded[urlString] = content
        return content
    }
}
