import ActivityKit
import Foundation

/// Starts, updates and ends the trip's Live Activity (lock screen and Dynamic Island; drawn by
/// the BikePodWidgets extension). Updates go out when what's shown changes, at most once a second.
@MainActor
final class TripActivityController {
    private var activity: Activity<TripActivityAttributes>?
    private var shown: TripActivityAttributes.ContentState?
    private var shownAt = Date.distantPast

    func start(destination: String, arrival: Date, kmLeft: Double) {
        end(arrived: false)
        guard ActivityAuthorizationInfo().areActivitiesEnabled else { return }
        let state = TripActivityAttributes.ContentState(symbol: "location.north.fill", distance: "", road: "Starting",
                                                        arrival: arrival, kmLeft: kmLeft, rerouting: false, arrived: false)
        do {
            activity = try Activity.request(attributes: TripActivityAttributes(destination: destination),
                                            content: ActivityContent(state: state, staleDate: nil))
            shown = state
        } catch {
            DebugLog.write("LIVE ACTIVITY not started: \(error.localizedDescription)")
        }
    }

    func update(_ state: TripActivityAttributes.ContentState) {
        guard let activity, state != shown else { return }
        // The turn or road changing goes at once; the distance counting down at most once a second
        let sameTurn = state.symbol == shown?.symbol && state.road == shown?.road && state.rerouting == shown?.rerouting
        guard !sameTurn || Date().timeIntervalSince(shownAt) >= 1 else { return }
        shown = state
        shownAt = Date()
        Task { await activity.update(ActivityContent(state: state, staleDate: Date().addingTimeInterval(120))) }
    }

    /// Arrived: shows "Arrived" for a few minutes; otherwise it goes at once
    func end(arrived: Bool, destinationState: TripActivityAttributes.ContentState? = nil) {
        guard let activity else { return }
        self.activity = nil
        shown = nil
        let final = destinationState.map { ActivityContent(state: $0, staleDate: nil) }
        Task { await activity.end(final, dismissalPolicy: arrived ? .after(Date().addingTimeInterval(5 * 60)) : .immediate) }
    }
}
