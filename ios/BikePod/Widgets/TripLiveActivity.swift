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
                    TurnImage(symbol: state.symbol)
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
                TurnImage(symbol: state.symbol)
                    .foregroundStyle(state.rerouting ? .orange : .blue)
            } compactTrailing: {
                Text(state.arrived ? "Here" : state.distance)
                    .font(.caption.weight(.semibold))
                    .monospacedDigit()
            } minimal: {
                TurnImage(symbol: state.symbol).foregroundStyle(.blue)
            }
        }
    }
}

/// The turn's SF Symbol. There is no right-hand U-turn symbol, so that one is the left one mirrored.
private struct TurnImage: View {
    let symbol: String

    var body: some View {
        if symbol == TripActivityAttributes.uTurnRight {
            Image(systemName: "arrow.uturn.down").scaleEffect(x: -1, y: 1)
        } else {
            Image(systemName: symbol)
        }
    }
}

private struct LockScreenView: View {
    let state: TripActivityAttributes.ContentState
    let destination: String

    var body: some View {
        HStack(spacing: 14) {
            TurnImage(symbol: state.symbol)
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
