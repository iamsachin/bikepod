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
    /// The area before the current one: the pod may still be showing it while the new one arrives
    private var previous: (id: Int, frame: MapArea.Frame)?
    private var building = false
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
                let content = await tileContent(template: template, x: t.x, y: t.y)
                roads += content.roads
                fuel += content.fuel
            }
            let nextID = areaID % 60_000 + 1
            let newFrame = MapArea.Frame(refLat: lat, refLng: lng)
            let lines = await Task.detached(priority: .utility) {
                MapArea.lines(id: nextID, frame: newFrame, roads: roads, fuel: fuel)
            }.value
            if let frame { previous = (areaID, frame) }
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

    /// The same pose in the previous area's frame, so the pod can keep drawing that area until
    /// the new one has fully arrived (otherwise the map blinks out at every area switch)
    func previousPose(lat: Double, lng: Double, headingDegrees: Double) -> String? {
        guard let previous else { return nil }
        return MapArea.pose(id: previous.id, frame: previous.frame, lat: lat, lng: lng, headingDegrees: headingDegrees)
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

    private func tileContent(template: String, x: Int, y: Int) async -> TileContent {
        let urlString = template.replacingOccurrences(of: "{z}", with: "\(Self.zoom)")
            .replacingOccurrences(of: "{x}", with: "\(x)").replacingOccurrences(of: "{y}", with: "\(y)")
        if let content = decoded[urlString] { return content }
        guard let url = URL(string: urlString) else { return TileContent(roads: [], fuel: []) }

        // Disk cache, named after the URL path (it includes the data version, so new data is refetched)
        let name = url.path.split(separator: "/").joined(separator: "_")
        let file = Self.cacheDir.appendingPathComponent(name)
        var data = try? Data(contentsOf: file)
        if data == nil, let (fresh, response) = try? await URLSession.shared.data(from: url),
           (response as? HTTPURLResponse)?.statusCode == 200 {
            data = fresh
            try? fresh.write(to: file)
        }
        guard let data else { return TileContent(roads: [], fuel: []) }
        let content = await Task.detached(priority: .utility) {
            TileContent(roads: VectorTile.roads(from: data, z: Self.zoom, x: x, y: y),
                        fuel: VectorTile.fuelStations(from: data, z: Self.zoom, x: x, y: y))
        }.value
        if decoded.count > 16 { decoded.removeAll() }
        decoded[urlString] = content
        return content
    }
}
