import CoreLocation
import Foundation
import Observation

struct Place: Identifiable, Hashable {
    let id: String
    let name: String
    let detail: String
    let latitude: Double
    let longitude: Double

    var coordinate: CLLocationCoordinate2D {
        CLLocationCoordinate2D(latitude: latitude, longitude: longitude)
    }
}

/// Place search with Photon (komoot's free OpenStreetMap geocoder).
/// Results are biased towards the rider's position. Public server: light personal use only,
/// so typing is debounced and only the latest query is kept.
@MainActor
@Observable
final class PlaceSearch {
    private(set) var results: [Place] = []
    private(set) var isSearching = false
    private(set) var error: String?

    @ObservationIgnored private var task: Task<Void, Never>?

    func search(_ query: String, near: CLLocationCoordinate2D?) {
        task?.cancel()
        let trimmed = query.trimmingCharacters(in: .whitespaces)
        guard trimmed.count >= 3 else {
            results = []
            isSearching = false
            return
        }
        task = Task {
            try? await Task.sleep(for: .milliseconds(400))   // wait for typing to pause
            guard !Task.isCancelled else { return }
            isSearching = true
            defer { if !Task.isCancelled { isSearching = false } }
            do {
                let places = try await Self.fetch(trimmed, near: near)
                guard !Task.isCancelled else { return }
                results = places
                error = nil
            } catch is CancellationError {
            } catch {
                guard !Task.isCancelled else { return }
                self.error = "Search failed: \(error.localizedDescription)"
            }
        }
    }

    func clear() {
        task?.cancel()
        results = []
        isSearching = false
        error = nil
    }

    private static func fetch(_ query: String, near: CLLocationCoordinate2D?) async throws -> [Place] {
        var components = URLComponents(string: "https://photon.komoot.io/api/")!
        var items = [URLQueryItem(name: "q", value: query), URLQueryItem(name: "limit", value: "8")]
        if let near {
            items += [URLQueryItem(name: "lat", value: String(near.latitude)),
                      URLQueryItem(name: "lon", value: String(near.longitude))]
        }
        components.queryItems = items
        var request = URLRequest(url: components.url!)
        request.setValue("BikePod/0.1 (personal project)", forHTTPHeaderField: "User-Agent")

        let (data, _) = try await URLSession.shared.data(for: request)
        let response = try JSONDecoder().decode(PhotonResponse.self, from: data)
        return response.features.compactMap { feature in
            let p = feature.properties
            guard feature.geometry.coordinates.count == 2, let name = p.name ?? p.street else { return nil }
            let detail = [p.street == name ? nil : p.street, p.district ?? p.locality, p.city]
                .compactMap { $0 }
                .joined(separator: ", ")
            return Place(id: "\(p.osm_type ?? "")\(p.osm_id ?? 0)",
                         name: name,
                         detail: detail,
                         latitude: feature.geometry.coordinates[1],
                         longitude: feature.geometry.coordinates[0])
        }
    }
}

private struct PhotonResponse: Decodable {
    struct Feature: Decodable {
        struct Geometry: Decodable { let coordinates: [Double] }   // [lon, lat]
        struct Properties: Decodable {
            let name: String?
            let street: String?
            let district: String?
            let locality: String?
            let city: String?
            let osm_type: String?
            let osm_id: Int?
        }
        let geometry: Geometry
        let properties: Properties
    }
    let features: [Feature]
}
