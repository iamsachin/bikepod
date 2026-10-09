import SwiftUI

/// Step 4a test screen: send turns to the pod by hand, or play a fake approach.
struct ContentView: View {
    let link: PodLink

    @State private var dir: TurnDir = .right
    @State private var metres: Double = 400
    @State private var road = "MG Road"
    @State private var approach: Task<Void, Never>?

    private var connected: Bool { link.state == .connected }

    var body: some View {
        NavigationStack {
            Form {
                Section("Pod") {
                    HStack {
                        Circle()
                            .fill(connected ? .green : .orange)
                            .frame(width: 10, height: 10)
                        Text(link.state.rawValue)
                    }
                    if !link.lastSent.isEmpty {
                        LabeledContent("Last sent", value: link.lastSent)
                            .font(.footnote.monospaced())
                    }
                }

                Section("Turn") {
                    Picker("Direction", selection: $dir) {
                        ForEach(TurnDir.allCases) { Text($0.label).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    VStack(alignment: .leading) {
                        Text("Distance: \(Int(metres)) m")
                        Slider(value: $metres, in: 0...2000, step: 10)
                    }
                    TextField("Road name", text: $road)
                }

                Section {
                    Button("Send to pod") {
                        link.send(dir, metres: Int(metres), road: road)
                    }
                    Button(approach == nil ? "Play approach (counts down to 0 m)" : "Stop approach") {
                        approach == nil ? startApproach() : stopApproach()
                    }
                }
                .disabled(!connected)
            }
            .navigationTitle("BikePod")
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
