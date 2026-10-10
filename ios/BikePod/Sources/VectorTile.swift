import Foundation

/// Road classes, from the OpenMapTiles `transportation` layer, grouped by how the pod draws them.
enum RoadClass: Int {
    case major = 1      // motorway, trunk, primary
    case medium = 2     // secondary, tertiary
    case minor = 3      // minor (residential streets)

    init?(openMapTilesClass value: String) {
        switch value {
        case "motorway", "trunk", "primary": self = .major
        case "secondary", "tertiary": self = .medium
        case "minor": self = .minor
        default: return nil     // service lanes, tracks, paths, rail...: clutter, not drawn
        }
    }
}

/// A road from a map tile, as latitude/longitude points.
struct MapRoad {
    let roadClass: RoadClass
    let points: [(lat: Double, lng: Double)]
}

/// Minimal decoder for Mapbox Vector Tiles (the .pbf tiles OpenFreeMap serves), reading only
/// road lines from the `transportation` layer. Format: https://github.com/mapbox/vector-tile-spec
enum VectorTile {
    static func roads(from data: Data, z: Int, x: Int, y: Int) -> [MapRoad] {
        var reader = ProtoReader(data)
        var roads: [MapRoad] = []
        while let (field, wire) = reader.nextKey() {
            if field == 3, wire == 2, let layer = reader.bytes() {        // Tile.layers
                roads += parseLayer(layer, z: z, x: x, y: y)
            } else {
                reader.skip(wire)
            }
        }
        return roads
    }

    private static func parseLayer(_ data: Data, z: Int, x: Int, y: Int) -> [MapRoad] {
        var reader = ProtoReader(data)
        var name = ""
        var features: [Data] = []
        var keys: [String] = []
        var values: [String?] = []
        var extent = 4096.0
        while let (field, wire) = reader.nextKey() {
            switch (field, wire) {
            case (1, 2): name = reader.string() ?? ""
            case (2, 2): if let f = reader.bytes() { features.append(f) }
            case (3, 2): keys.append(reader.string() ?? "")
            case (4, 2): values.append(reader.bytes().flatMap(stringValue))
            case (5, 0): extent = Double(reader.varint() ?? 4096)
            default: reader.skip(wire)
            }
        }
        guard name == "transportation", let classKey = keys.firstIndex(of: "class") else { return [] }

        let tiles = Double(1 << z)
        func latLng(_ px: Double, _ py: Double) -> (lat: Double, lng: Double) {
            let wx = (Double(x) + px / extent) / tiles
            let wy = (Double(y) + py / extent) / tiles
            return (atan(sinh(.pi * (1 - 2 * wy))) * 180 / .pi, wx * 360 - 180)
        }

        var roads: [MapRoad] = []
        for feature in features {
            var reader = ProtoReader(feature)
            var tags: [Int] = []
            var type = 0
            var geometry: [Int] = []
            while let (field, wire) = reader.nextKey() {
                switch (field, wire) {
                case (2, 2): tags = reader.packedVarints()
                case (3, 0): type = reader.varint() ?? 0
                case (4, 2): geometry = reader.packedVarints()
                default: reader.skip(wire)
                }
            }
            guard type == 2 else { continue }                           // lines only
            var roadClass: RoadClass?
            for i in stride(from: 0, to: tags.count - 1, by: 2) where tags[i] == classKey {
                if tags[i + 1] < values.count, let value = values[tags[i + 1]] {
                    roadClass = RoadClass(openMapTilesClass: value)
                }
            }
            guard let roadClass else { continue }

            // Geometry commands: MoveTo(1) / LineTo(2) with zig-zag encoded deltas
            var cx = 0, cy = 0, i = 0
            var line: [(x: Double, y: Double)] = []
            func finish() {
                for piece in clipped(line, extent: extent) {
                    roads.append(MapRoad(roadClass: roadClass, points: piece.map { latLng($0.x, $0.y) }))
                }
                line = []
            }
            while i < geometry.count {
                let command = geometry[i] & 0x7, count = geometry[i] >> 3
                i += 1
                if command == 1 { finish() }
                guard command == 1 || command == 2 else { continue }
                for _ in 0..<count where i + 1 < geometry.count {
                    cx += zigzag(geometry[i]); cy += zigzag(geometry[i + 1]); i += 2
                    line.append((Double(cx), Double(cy)))
                }
            }
            finish()
        }
        return roads
    }

    /// The parts of a line inside the tile. Tiles repeat a margin of their neighbours' roads, so
    /// without clipping every road crossing a tile edge was sent (and drawn) twice there.
    private static func clipped(_ line: [(x: Double, y: Double)], extent: Double) -> [[(x: Double, y: Double)]] {
        var pieces: [[(x: Double, y: Double)]] = []
        var run: [(x: Double, y: Double)] = []
        for k in 1..<max(line.count, 1) {
            let a = line[k - 1], b = line[k]
            // Liang-Barsky: the part of segment a-b inside [0, extent] x [0, extent]
            var t0 = 0.0, t1 = 1.0
            let dx = b.x - a.x, dy = b.y - a.y
            var inside = true
            for (p, q) in [(-dx, a.x), (dx, extent - a.x), (-dy, a.y), (dy, extent - a.y)] {
                if p == 0 {
                    if q < 0 { inside = false }
                } else {
                    let r = q / p
                    if p < 0 { t0 = max(t0, r) } else { t1 = min(t1, r) }
                }
            }
            guard inside, t0 <= t1 else {
                if run.count >= 2 { pieces.append(run) }
                run = []
                continue
            }
            let start = (x: a.x + t0 * dx, y: a.y + t0 * dy), end = (x: a.x + t1 * dx, y: a.y + t1 * dy)
            if run.isEmpty || t0 > 0 {
                if run.count >= 2 { pieces.append(run) }
                run = [start]
            }
            run.append(end)
            if t1 < 1 {
                pieces.append(run)
                run = []
            }
        }
        if run.count >= 2 { pieces.append(run) }
        return pieces.filter { $0.count >= 2 }
    }

    /// Fuel stations (OpenStreetMap amenity=fuel) from the `poi` layer, as points
    static func fuelStations(from data: Data, z: Int, x: Int, y: Int) -> [(lat: Double, lng: Double)] {
        var reader = ProtoReader(data)
        var stations: [(lat: Double, lng: Double)] = []
        while let (field, wire) = reader.nextKey() {
            guard field == 3, wire == 2, let layer = reader.bytes() else { reader.skip(wire); continue }
            var lr = ProtoReader(layer)
            var name = ""
            var features: [Data] = []
            var keys: [String] = []
            var values: [String?] = []
            var extent = 4096.0
            while let (f, w) = lr.nextKey() {
                switch (f, w) {
                case (1, 2): name = lr.string() ?? ""
                case (2, 2): if let feat = lr.bytes() { features.append(feat) }
                case (3, 2): keys.append(lr.string() ?? "")
                case (4, 2): values.append(lr.bytes().flatMap(stringValue))
                case (5, 0): extent = Double(lr.varint() ?? 4096)
                default: lr.skip(w)
                }
            }
            guard name == "poi", let classKey = keys.firstIndex(of: "class") else { continue }
            let tiles = Double(1 << z)
            for feature in features {
                var fr = ProtoReader(feature)
                var tags: [Int] = []
                var type = 0
                var geometry: [Int] = []
                while let (f, w) = fr.nextKey() {
                    switch (f, w) {
                    case (2, 2): tags = fr.packedVarints()
                    case (3, 0): type = fr.varint() ?? 0
                    case (4, 2): geometry = fr.packedVarints()
                    default: fr.skip(w)
                    }
                }
                guard type == 1, geometry.count >= 3 else { continue }       // points only
                var isFuel = false
                for i in stride(from: 0, to: tags.count - 1, by: 2) where tags[i] == classKey {
                    isFuel = tags[i + 1] < values.count && values[tags[i + 1]] == "fuel"
                }
                guard isFuel else { continue }
                let px = Double(zigzag(geometry[1])), py = Double(zigzag(geometry[2]))
                let wx = (Double(x) + px / extent) / tiles, wy = (Double(y) + py / extent) / tiles
                stations.append((atan(sinh(.pi * (1 - 2 * wy))) * 180 / .pi, wx * 360 - 180))
            }
        }
        return stations
    }

    private static func stringValue(_ data: Data) -> String? {
        var reader = ProtoReader(data)
        while let (field, wire) = reader.nextKey() {
            if field == 1, wire == 2 { return reader.string() }
            reader.skip(wire)
        }
        return nil
    }

    private static func zigzag(_ n: Int) -> Int { (n >> 1) ^ -(n & 1) }

    /// Which zoom-z tiles cover a box around a point
    static func tiles(around lat: Double, _ lng: Double, radiusMetres: Double, z: Int) -> [(x: Int, y: Int)] {
        let dLat = radiusMetres / 110_540, dLng = radiusMetres / (111_320 * cos(lat * .pi / 180))
        func tile(_ la: Double, _ ln: Double) -> (Int, Int) {
            let n = Double(1 << z)
            let x = Int((ln + 180) / 360 * n)
            let y = Int((1 - log(tan(la * .pi / 180) + 1 / cos(la * .pi / 180)) / .pi) / 2 * n)
            return (x, y)
        }
        let (x0, y0) = tile(lat + dLat, lng - dLng), (x1, y1) = tile(lat - dLat, lng + dLng)
        return (x0...x1).flatMap { x in (y0...y1).map { y in (x, y) } }
    }
}

/// Just enough protobuf reading for vector tiles.
private struct ProtoReader {
    private let data: [UInt8]
    private var pos = 0
    init(_ data: Data) { self.data = [UInt8](data) }

    mutating func varint() -> Int? {
        var result = 0, shift = 0
        while pos < data.count {
            let b = data[pos]; pos += 1
            result |= Int(b & 0x7F) << shift
            if b & 0x80 == 0 { return result }
            shift += 7
            if shift > 63 { return nil }
        }
        return nil
    }

    mutating func nextKey() -> (field: Int, wire: Int)? {
        guard pos < data.count, let key = varint() else { return nil }
        return (key >> 3, key & 0x7)
    }

    mutating func bytes() -> Data? {
        guard let length = varint(), length >= 0, pos + length <= data.count else { pos = data.count; return nil }
        defer { pos += length }
        return Data(data[pos..<(pos + length)])
    }

    mutating func string() -> String? { bytes().flatMap { String(data: $0, encoding: .utf8) } }

    mutating func packedVarints() -> [Int] {
        guard let packed = bytes() else { return [] }
        var inner = ProtoReader(packed)
        var out: [Int] = []
        while let v = inner.varint() { out.append(v) }
        return out
    }

    mutating func skip(_ wire: Int) {
        switch wire {
        case 0: _ = varint()
        case 1: pos += 8
        case 2: _ = bytes()
        case 5: pos += 4
        default: pos = data.count
        }
    }
}
