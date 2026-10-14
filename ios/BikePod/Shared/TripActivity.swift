import ActivityKit
import Foundation

/// The trip shown on the lock screen and in the Dynamic Island while navigating (Live Activity).
/// Shared by the app, which starts and updates it, and the widget extension, which draws it.
struct TripActivityAttributes: ActivityAttributes {
    struct ContentState: Codable, Hashable {
        /// SF Symbol for the next maneuver
        var symbol: String
        /// "350 m", "1.2 km"
        var distance: String
        var road: String
        var arrival: Date
        var kmLeft: Double
        var rerouting: Bool
        var arrived: Bool
    }

    var destination: String
}
