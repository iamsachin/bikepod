import SwiftUI

/// Pod status, ride simulation, and manual controls: send turns to the pod by hand, or play a
/// fake approach.
struct PodTestView: View {
    let link: PodLink
    @Bindable var navigator: Navigator
    @Environment(\.dismiss) private var dismiss

    @State private var dir: TurnDir = .right
    @State private var metres: Double = 400
    @State private var road = "MG Road"
    @State private var approach: Task<Void, Never>?

    private var connected: Bool { link.state == .connected }

    var body: some View {
        NavigationStack {
            Form {
                Section("Pod") {
                    PodStatusLabel(link: link)
                    if !link.lastSent.isEmpty {
                        LabeledContent("Last sent", value: link.lastSent)
                            .font(.footnote.monospaced())
                    }
                }
                Section {
                    Toggle("Simulate rides", isOn: $navigator.simulateRides)
                    if navigator.simulateRides {
                        Picker("Speed", selection: $navigator.simulationSpeed) {
                            Text("1×").tag(UInt64(1))
                            Text("4×").tag(UInt64(4))
                            Text("10×").tag(UInt64(10))
                        }
                        .pickerStyle(.segmented)
                    }
                } header: {
                    Text("Ride simulation")
                } footer: {
                    Text("Replays the route with fake GPS, for testing at a desk. Turns off when the app restarts.")
                }
                Section("Manual test") {
                    Picker("Direction", selection: $dir) {
                        ForEach(TurnDir.allCases) { Text($0.label).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    VStack(alignment: .leading) {
                        Text("Distance: \(Int(metres)) m")
                        Slider(value: $metres, in: 0...2000, step: 10)
                    }
                    TextField("Road name", text: $road)
                    Button("Send to pod") {
                        link.send(dir, metres: Int(metres), road: road)
                    }
                    Button(approach == nil ? "Play approach (counts down to 0 m)" : "Stop approach") {
                        approach == nil ? startApproach() : stopApproach()
                    }
                }
                .disabled(!connected)
            }
            .navigationTitle("Pod & testing")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                Button("Done") { dismiss() }
            }
            .onDisappear { stopApproach() }
        }
    }

    /// Pretend to ride towards the turn: about 15 m/s (54 km/h), one update every half second.
    private func startApproach() {
        let start = Int(metres)
        approach = Task {
            var remaining = start
            while remaining > 0, !Task.isCancelled {
                link.send(dir, metres: remaining, road: road)
                try? await Task.sleep(for: .milliseconds(500))
                remaining -= 8
            }
            if !Task.isCancelled { link.send(dir, metres: 0, road: road) }
            approach = nil
        }
    }

    private func stopApproach() {
        approach?.cancel()
        approach = nil
    }
}

/// "● BikePod  Connected" as a small capsule, for the map screens
struct PodPill: View {
    let link: PodLink

    var body: some View {
        HStack(spacing: 6) {
            Circle()
                .fill(link.state == .connected ? Color.green : Color.orange)
                .frame(width: 8, height: 8)
            Text("BikePod").fontWeight(.semibold)
            Text(link.state == .connected ? "Connected" : link.state == .bluetoothOff ? "Bluetooth off" : "Searching…")
                .foregroundStyle(.secondary)
        }
        .font(.footnote)
        .lineLimit(1)
        .fixedSize()
        .padding(.horizontal, 12)
        .padding(.vertical, 7)
        .background(.regularMaterial, in: Capsule())
        .accessibilityElement(children: .combine)
    }
}

struct PodStatusLabel: View {
    let link: PodLink

    var body: some View {
        HStack(spacing: 6) {
            Circle()
                .fill(link.state == .connected ? .green : .orange)
                .frame(width: 8, height: 8)
            Text(link.state == .connected ? "Pod connected" : link.state.rawValue)
        }
    }
}
