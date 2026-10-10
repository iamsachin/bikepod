import CoreLocation
import FerrostarCoreFFI
import Foundation
import Observation

/// A finished ride, as kept in the ride history
struct Ride: Codable, Identifiable, Hashable {
    let id: UUID
    var start: Date
    var end: Date
    var destination: String
    var metres: Double
    /// Time actually moving (stops at lights and in traffic left out), for the average speed
    var movingSeconds: Double
    var topSpeedKmh: Double
    /// Deepest lean each side, from the pod (nil when the pod wasn't connected)
    var maxLeanLeft: Int?
    var maxLeanRight: Int?
    /// Reached the destination (rather than ending the ride early)
    var arrived: Bool
    /// A replayed route, not a real ride
    var simulated: Bool

    var km: Double { metres / 1000 }
    var seconds: TimeInterval { end.timeIntervalSince(start) }
    var averageKmh: Double? { movingSeconds > 30 ? metres / movingSeconds * 3.6 : nil }
}

/// Records the ride in progress: points along the way for the map, and the numbers
@MainActor
final class RideRecorder {
    private(set) var ride: Ride
    private(set) var track: [CLLocationCoordinate2D] = []
    private var lastPoint: CLLocation?
    private var lastUpdate: Date?

    init(destination: String, simulated: Bool) {
        let now = Date()
        ride = Ride(id: UUID(), start: now, end: now, destination: destination, metres: 0, movingSeconds: 0,
                    topSpeedKmh: 0, maxLeanLeft: nil, maxLeanRight: nil, arrived: false, simulated: simulated)
    }

    func add(_ location: UserLocation) {
        // Rough fixes (in tunnels, between tall buildings) would add zigzags and distance
        guard ride.simulated || location.horizontalAccuracy <= 30 else { return }
        let now = Date()
        let speed = max(0, location.speed?.value ?? 0)                 // metres per second
        if let lastUpdate, speed >= 1 {
            // Capped: after a gap in GPS, don't count time that can't be vouched for
            ride.movingSeconds += min(now.timeIntervalSince(lastUpdate), 5)
        }
        lastUpdate = now
        if speed * 3.6 < 250 { ride.topSpeedKmh = max(ride.topSpeedKmh, speed * 3.6) }   // not GPS glitches
        // A point every 10 m or more is plenty for the map, and keeps standing GPS jitter out
        let here = CLLocation(latitude: location.coordinates.lat, longitude: location.coordinates.lng)
        if let lastPoint {
            let step = here.distance(from: lastPoint)
            guard step >= 10 else { return }
            ride.metres += step
        }
        lastPoint = here
        track.append(here.coordinate)
        ride.end = now
    }

    /// The pod's deepest lean each side in the last couple of seconds
    func lean(left: Int, right: Int) {
        ride.maxLeanLeft = max(ride.maxLeanLeft ?? 0, left)
        ride.maxLeanRight = max(ride.maxLeanRight ?? 0, right)
    }

    func finish(arrived: Bool) -> Ride {
        ride.end = Date()
        ride.arrived = arrived
        return ride
    }
}

/// Past rides, kept on the phone: a list of rides (rides.json) plus each ride's track in its own
/// file, so the list opens quickly however long the rides were
@MainActor
@Observable
final class RideHistory {
    private(set) var rides: [Ride] = []                      // newest first

    private static let folder: URL = {
        let url = URL.documentsDirectory.appending(path: "Rides", directoryHint: .isDirectory)
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }()
    private static var indexURL: URL { folder.appending(path: "rides.json") }
    private static func trackURL(_ id: UUID) -> URL { folder.appending(path: "\(id.uuidString).track") }

    init() {
        if let data = try? Data(contentsOf: Self.indexURL),
           let saved = try? JSONDecoder().decode([Ride].self, from: data) {
            rides = saved.sorted { $0.start > $1.start }
        }
    }

    /// Keeps a finished ride (very short ones, like starting by mistake, are dropped)
    func save(_ ride: Ride, track: [CLLocationCoordinate2D]) {
        guard ride.metres >= 200, track.count >= 2 else {
            DebugLog.write(String(format: "RIDE not kept (%.0f m)", ride.metres))
            return
        }
        // Track: latitude, longitude pairs as 32-bit floats (about 1 m precision, 8 bytes a point)
        var floats: [Float32] = []
        floats.reserveCapacity(track.count * 2)
        for point in track { floats += [Float32(point.latitude), Float32(point.longitude)] }
        let data = floats.withUnsafeBufferPointer { Data(buffer: $0) }
        try? data.write(to: Self.trackURL(ride.id), options: .atomic)
        rides.insert(ride, at: 0)
        writeIndex()
        DebugLog.write(String(format: "RIDE kept: %.1f km, %d points", ride.km, track.count))
    }

    func track(for ride: Ride) -> [CLLocationCoordinate2D] {
        guard let data = try? Data(contentsOf: Self.trackURL(ride.id)) else { return [] }
        let floats = data.withUnsafeBytes { Array($0.bindMemory(to: Float32.self)) }
        return stride(from: 0, to: floats.count - 1, by: 2).map {
            CLLocationCoordinate2D(latitude: Double(floats[$0]), longitude: Double(floats[$0 + 1]))
        }
    }

    func delete(_ ride: Ride) {
        rides.removeAll { $0.id == ride.id }
        try? FileManager.default.removeItem(at: Self.trackURL(ride.id))
        writeIndex()
    }

    private func writeIndex() {
        if let data = try? JSONEncoder().encode(rides) { try? data.write(to: Self.indexURL, options: .atomic) }
    }
}
