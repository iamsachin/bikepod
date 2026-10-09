import FerrostarCore
import Foundation

/// A junction on the route: where it is in the route's coordinate list, and which roads meet there.
struct Junction {
    let geometryIndex: Int
    /// Compass bearings (degrees clockwise from north) of every road leaving the junction
    let bearings: [Double]
    let inIndex: Int?     // bearings[inIndex] points back along the road we arrive on
    let outIndex: Int?    // bearings[outIndex] is the road we leave by

    /// Bearings of the side streets: everything except the roads we arrive and leave on, and
    /// anything within 30 degrees of those (it would be drawn as a ghost line beside the route)
    var sideBearings: [Double] {
        let route = [inIndex, outIndex].compactMap { $0 }.filter { bearings.indices.contains($0) }.map { bearings[$0] }
        return bearings.enumerated()
            .filter { $0.offset != inIndex && $0.offset != outIndex }
            .map(\.element)
            .filter { side in route.allSatisfy { Self.angleBetween(side, $0) >= 30 } }
    }

    private static func angleBetween(_ a: Double, _ b: Double) -> Double {
        let d = abs(a - b).truncatingRemainder(dividingBy: 360)
        return min(d, 360 - d)
    }
}

/// Network loader for Ferrostar that also keeps the junction details from Valhalla's answer.
///
/// Ferrostar turns Valhalla's response into its own Route type and drops the `intersections`
/// lists, which the pod needs to draw side streets. Every route request (first route and
/// every reroute) passes through here, so we read them from the same bytes Ferrostar parses.
final class JunctionCapture: URLRequestLoading, @unchecked Sendable {
    private let lock = NSLock()
    private var junctionsByRoute: [Int: [Junction]] = [:]   // keyed by route distance, whole metres

    func loadData(with urlRequest: URLRequest) async throws -> (Data, URLResponse) {
        let (data, response) = try await URLSession.shared.data(for: urlRequest)
        if let parsed = Self.parse(data) {
            lock.withLock {
                if junctionsByRoute.count > 20 { junctionsByRoute.removeAll() }
                junctionsByRoute.merge(parsed) { _, new in new }
            }
        }
        return (data, response)
    }

    /// Junctions of the route with this length (Route.distance), if we saw it come in
    func junctions(forRouteDistance distance: Double) -> [Junction] {
        lock.withLock { junctionsByRoute[Int(distance.rounded())] ?? [] }
    }

    private static func parse(_ data: Data) -> [Int: [Junction]]? {
        guard let response = try? JSONDecoder().decode(OSRMResponse.self, from: data) else { return nil }
        var result: [Int: [Junction]] = [:]
        for route in response.routes {
            let junctions = route.legs.flatMap(\.steps).flatMap { $0.intersections ?? [] }.compactMap { i -> Junction? in
                guard let index = i.geometry_index else { return nil }
                return Junction(geometryIndex: index, bearings: i.bearings, inIndex: i.in, outIndex: i.out)
            }
            result[Int(route.distance.rounded())] = junctions
        }
        return result
    }
}

private struct OSRMResponse: Decodable {
    struct Route: Decodable {
        let distance: Double
        let legs: [Leg]
    }
    struct Leg: Decodable { let steps: [Step] }
    struct Step: Decodable { let intersections: [Intersection]? }
    struct Intersection: Decodable {
        let bearings: [Double]
        let `in`: Int?
        let out: Int?
        let geometry_index: Int?
    }
    let routes: [Route]
}
