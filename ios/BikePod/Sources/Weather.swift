import CoreLocation
import FerrostarCoreFFI
import Foundation

/// Rain along the route ahead, from Open-Meteo's 15-minute forecast: when the rider will meet rain
/// (minutes from now), or that it's raining where they are. Open-Meteo is free without a key for
/// personal use; with a key (Settings, kept in the Keychain) its customer service is used instead.
@MainActor
final class Weather {
    static let keyName = "openMeteoKey"

    /// Minutes until rain on the route: 0 = raining now, nil = none in the next ~2 hours or not known yet
    private(set) var minutesToRain: Int?
    private(set) var known = false
    private var checkedAt = Date.distantPast
    private var task: Task<Void, Never>?

    /// Called with each navigation update; checks again every 10 minutes
    func update(rider: GeographicCoordinate, route: [GeographicCoordinate], along: Double,
                metresLeft: Double, secondsLeft: Double) {
        guard Date().timeIntervalSince(checkedAt) > 600, task == nil, route.count >= 2 else { return }
        checkedAt = Date()
        // The rider now, then every 5 km ahead (up to ~2 hours), each with when they'll be there
        let speed = secondsLeft > 0 ? metresLeft / secondsLeft : 10
        var points: [(lat: Double, lng: Double, seconds: Double)] = [(rider.lat, rider.lng, 0)]
        var ahead = 5_000.0
        while ahead < metresLeft, ahead / speed < 2 * 3600, points.count < 12 {
            if let p = Hazards.coordinate(on: route, atAlong: along + ahead) { points.append((p.lat, p.lng, ahead / speed)) }
            ahead += 5_000
        }
        task = Task { [weak self] in
            let result = await Self.fetch(points)
            guard let self else { return }
            if let result {
                self.minutesToRain = result
                self.known = true
                DebugLog.write("WEATHER rain \(result.map { "in \($0) min" } ?? "none") (\(points.count) points)")
            } else {
                self.checkedAt = Date().addingTimeInterval(-480)       // failed: try again in 2 minutes
            }
            self.task = nil
        }
    }

    func clear() {
        task?.cancel()
        task = nil
        minutesToRain = nil
        known = false
        checkedAt = .distantPast
    }

    /// Minutes until rain (0 = now), .some(nil) = dry, nil = couldn't ask
    private static func fetch(_ points: [(lat: Double, lng: Double, seconds: Double)]) async -> Int?? {
        let key = Keychain.string(keyName)
        var components = URLComponents(string: key == nil ? "https://api.open-meteo.com/v1/forecast"
                                                          : "https://customer-api.open-meteo.com/v1/forecast")!
        components.queryItems = [
            URLQueryItem(name: "latitude", value: points.map { String(format: "%.4f", $0.lat) }.joined(separator: ",")),
            URLQueryItem(name: "longitude", value: points.map { String(format: "%.4f", $0.lng) }.joined(separator: ",")),
            URLQueryItem(name: "minutely_15", value: "precipitation"),
            URLQueryItem(name: "forecast_minutely_15", value: "12"),          // 3 hours
            URLQueryItem(name: "timeformat", value: "unixtime"),
        ]
        if let key { components.queryItems?.append(URLQueryItem(name: "apikey", value: key)) }
        struct Forecast: Decodable {
            struct Slots: Decodable { let time: [Double]; let precipitation: [Double?] }
            let minutely_15: Slots
        }
        guard let url = components.url, let (data, response) = try? await URLSession.shared.data(from: url),
              (response as? HTTPURLResponse)?.statusCode == 200 else { return nil }
        // One location gives an object, several give an array
        let forecasts = (try? JSONDecoder().decode([Forecast].self, from: data))
            ?? (try? JSONDecoder().decode(Forecast.self, from: data)).map { [$0] }
        guard let forecasts, forecasts.count == points.count else { return nil }
        let now = Date().timeIntervalSince1970
        for (point, forecast) in zip(points, forecasts) {
            let when = now + point.seconds
            let slots = forecast.minutely_15
            // The 15-minute slot the rider is in when they get there: 0.2 mm in 15 min is light rain
            guard let i = slots.time.lastIndex(where: { $0 <= when }), i < slots.precipitation.count,
                  (slots.precipitation[i] ?? 0) >= 0.2 else { continue }
            return .some(Int((point.seconds / 60).rounded()))
        }
        return .some(nil)
    }
}
