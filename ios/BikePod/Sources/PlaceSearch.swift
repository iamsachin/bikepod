import CoreLocation
import Foundation
import Observation

struct Place: Identifiable, Hashable, Codable {
    let id: String
    let name: String
    let detail: String
    let latitude: Double
    let longitude: Double
    /// OpenStreetMap type, e.g. "amenity=fuel" (nil for places saved before this was kept)
    var kind: String? = nil

    var coordinate: CLLocationCoordinate2D {
        CLLocationCoordinate2D(latitude: latitude, longitude: longitude)
    }

    /// SF Symbol for the kind of place
    var symbol: String {
        guard let kind else { return "mappin.circle" }
        let value = kind.split(separator: "=").last.map(String.init) ?? ""
        switch value {
        case "fuel", "charging_station": return "fuelpump.circle"
        case "restaurant", "cafe", "fast_food", "food_court", "bar", "pub": return "fork.knife.circle"
        case "hospital", "clinic", "doctors", "pharmacy": return "cross.circle"
        case "hotel", "guest_house", "hostel", "motel": return "bed.double.circle"
        case "park", "garden", "nature_reserve", "lake", "water": return "leaf.circle"
        case "school", "college", "university": return "graduationcap.circle"
        case "station", "bus_station", "bus_stop", "halt": return "tram.circle"
        case "aerodrome", "airport": return "airplane.circle"
        case "mall", "supermarket", "marketplace": return "bag.circle"
        default: break
        }
        if kind.hasPrefix("highway=") { return "signpost.right.circle" }
        if kind.hasPrefix("shop=") { return "bag.circle" }
        if kind.hasPrefix("place=") || kind.hasPrefix("boundary=") { return "building.2.crop.circle" }
        return "mappin.circle"
    }
}

/// Home and Work, kept on the phone between launches.
@MainActor
@Observable
final class SavedPlaces {
    enum Slot: String, CaseIterable, Identifiable {
        case home = "Home", work = "Work"
        var id: String { rawValue }
        var symbol: String { self == .home ? "house.fill" : "briefcase.fill" }
    }

    private(set) var places: [Slot: Place] = [:]

    init() {
        for slot in Slot.allCases {
            if let data = UserDefaults.standard.data(forKey: Self.key(slot)),
               let place = try? JSONDecoder().decode(Place.self, from: data) {
                places[slot] = place
            }
        }
    }

    func set(_ slot: Slot, to place: Place?) {
        places[slot] = place
        UserDefaults.standard.set(place.flatMap { try? JSONEncoder().encode($0) }, forKey: Self.key(slot))
    }

    private static func key(_ slot: Slot) -> String { "savedPlace.\(slot.rawValue)" }
}

/// Places picked from search, newest first, kept on the phone between launches.
@MainActor
@Observable
final class RecentPlaces {
    private static let key = "recentPlaces"
    private static let limit = 8
    private(set) var places: [Place]

    init() {
        let data = UserDefaults.standard.data(forKey: Self.key)
        places = data.flatMap { try? JSONDecoder().decode([Place].self, from: $0) } ?? []
    }

    func remember(_ place: Place) {
        places.removeAll { $0.id == place.id || ($0.name == place.name && $0.detail == place.detail) }
        places.insert(place, at: 0)
        places = Array(places.prefix(Self.limit))
        save()
    }

    func forget(_ place: Place) {
        places.removeAll { $0.id == place.id }
        save()
    }

    private func save() {
        UserDefaults.standard.set(try? JSONEncoder().encode(places), forKey: Self.key)
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
                         longitude: feature.geometry.coordinates[0],
                         kind: p.osm_key.map { "\($0)=\(p.osm_value ?? "")" })
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
            let osm_key: String?
            let osm_value: String?
        }
        let geometry: Geometry
        let properties: Properties
    }
    let features: [Feature]
}
