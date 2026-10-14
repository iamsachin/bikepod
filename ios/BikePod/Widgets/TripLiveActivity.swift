import ActivityKit
import SwiftUI
import WidgetKit

/// The trip on the lock screen and in the Dynamic Island: the next turn (arrow, distance, road)
/// and the arrival time, like the pod's turn screen
struct TripLiveActivity: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: TripActivityAttributes.self) { context in
            LockScreenView(state: context.state, destination: context.attributes.destination)
                .activityBackgroundTint(Color.black.opacity(0.85))
                .activitySystemActionForegroundColor(.white)
        } dynamicIsland: { context in
            let state = context.state
            return DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    Image(systemName: state.symbol)
                        .font(.system(size: 34, weight: .bold))
                        .foregroundStyle(state.rerouting ? .orange : .blue)
                        .padding(.leading, 4)
                }
                DynamicIslandExpandedRegion(.center) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(state.rerouting ? "Rerouting" : state.distance)
                            .font(.system(size: 26, weight: .bold, design: .rounded))
                        Text(state.road).font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                DynamicIslandExpandedRegion(.trailing) {
                    VStack(alignment: .trailing, spacing: 2) {
                        Text(state.arrival, style: .time).font(.subheadline.weight(.semibold))
                        Text(String(format: "%.1f km", state.kmLeft)).font(.caption).foregroundStyle(.secondary)
                    }
                    .padding(.trailing, 4)
                }
            } compactLeading: {
                Image(systemName: state.symbol)
                    .foregroundStyle(state.rerouting ? .orange : .blue)
            } compactTrailing: {
                Text(state.arrived ? "Here" : state.distance)
                    .font(.caption.weight(.semibold))
                    .monospacedDigit()
            } minimal: {
                Image(systemName: state.symbol).foregroundStyle(.blue)
            }
        }
    }
}

private struct LockScreenView: View {
    let state: TripActivityAttributes.ContentState
    let destination: String

    var body: some View {
        HStack(spacing: 14) {
            Image(systemName: state.symbol)
                .font(.system(size: 38, weight: .bold))
                .foregroundStyle(state.rerouting ? .orange : state.arrived ? .red : .blue)
                .frame(width: 48)
            VStack(alignment: .leading, spacing: 2) {
                Text(state.arrived ? "Arrived" : state.rerouting ? "Rerouting" : state.distance)
                    .font(.system(size: 28, weight: .bold, design: .rounded))
                    .foregroundStyle(.white)
                Text(state.arrived ? destination : state.road)
                    .font(.subheadline)
                    .foregroundStyle(.white.opacity(0.7))
                    .lineLimit(1)
            }
            Spacer()
            if !state.arrived {
                VStack(alignment: .trailing, spacing: 2) {
                    Text(state.arrival, style: .time)
                        .font(.headline)
                        .foregroundStyle(.white)
                    Text(String(format: "%.1f km", state.kmLeft))
                        .font(.caption)
                        .foregroundStyle(.white.opacity(0.7))
                }
            }
        }
        .padding(16)
    }
}
