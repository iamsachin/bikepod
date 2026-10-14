import CoreLocation
import FerrostarCore
import FerrostarCoreFFI
import FerrostarSwiftUI
import SwiftUI

/// The turn banner at the top of the navigation map. Ferrostar's own draws every U-turn swinging
/// left (its one U-turn icon is American); this is the same banner with the icon mirrored where
/// traffic keeps left, so a U-turn in India swings right as it does on the road. Tap it to see
/// the turns after.
struct TurnBanner: View {
    @Environment(\.navigationFormatterCollection) private var formatters: any FormatterCollection
    let navigationState: NavigationState?
    @Binding var isExpanded: Bool
    @Binding var sizeWhenNotExpanded: CGSize

    private let primaryTheme = DefaultInstructionRowTheme()
    private let secondaryTheme = DefaultSecondaryInstructionRowTheme()

    var body: some View {
        if case .navigating = navigationState?.tripState,
           let instruction = navigationState?.currentVisualInstruction,
           let progress = navigationState?.currentProgress,
           let steps = navigationState?.remainingSteps {
            let side = steps.first?.drivingSide
            VStack(spacing: 0) {
                VStack(spacing: 0) {
                    row(instruction.primaryContent, side: side, distance: progress.distanceToNextManeuver, theme: primaryTheme)
                        .font(.title2.bold())
                        .padding(.top, 16)
                        .padding(.bottom, instruction.secondaryContent == nil ? 16 : 8)
                    if let secondary = instruction.secondaryContent {
                        row(secondary, side: side, distance: nil, theme: secondaryTheme)
                            .padding(.vertical, 8)
                            .padding(.bottom, 8)
                            .background(secondaryTheme.backgroundColor)
                    }
                }
                .background(GeometryReader { geometry in
                    Color.clear
                        .onAppear { sizeWhenNotExpanded = geometry.size }
                        .onChange(of: geometry.size) { _, size in sizeWhenNotExpanded = size }
                        .onDisappear { sizeWhenNotExpanded = .zero }
                })

                if steps.count > 1 {
                    if isExpanded {
                        ScrollView {
                            ForEach(Array(steps.dropFirst().enumerated()), id: \.offset) { _, step in
                                if let next = step.visualInstructions.first?.primaryContent {
                                    Divider().padding(.leading, 16)
                                    row(next, side: step.drivingSide, distance: step.distance, theme: primaryTheme)
                                        .font(.title2.bold())
                                        .padding(.vertical, 8)
                                }
                            }
                        }
                        .frame(idealHeight: .infinity)
                    }
                    Group {
                        if isExpanded {
                            Image(systemName: "chevron.up").font(.system(size: 24, weight: .bold))
                        } else {
                            Capsule().frame(width: 40, height: 6)
                        }
                    }
                    .foregroundStyle(.gray.opacity(0.7))
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 8)
                }
            }
            .background(primaryTheme.backgroundColor)
            .cornerRadius(12)
            .shadow(radius: 12)
            .contentShape(Rectangle())
            .onTapGesture {
                guard steps.count > 1 else { return }
                withAnimation(.easeInOut(duration: 0.2)) { isExpanded.toggle() }
            }
        }
    }

    private func row(_ content: VisualInstructionContent, side: DrivingSide?, distance: CLLocationDistance?,
                     theme: InstructionRowTheme) -> some View {
        ManeuverInstructionView(text: content.text, distanceFormatter: formatters.distanceFormatter,
                                distanceToNextManeuver: distance, theme: theme) {
            if let type = content.maneuverType {
                ManeuverImage(maneuverType: type, maneuverModifier: content.maneuverModifier)
                    .frame(maxWidth: 48)
                    .aspectRatio(contentMode: .fill)
                    // The icon swings left; where traffic keeps left a U-turn swings right
                    .scaleEffect(x: content.maneuverModifier == .uTurn && side != .right ? -1 : 1, y: 1)
            }
        }
        .padding(.horizontal, 16)
    }
}
