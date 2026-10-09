import CoreLocation
import SwiftUI

/// Step 4b screen: navigate to a destination (real or simulated GPS),
/// plus the step 4a manual controls for testing the pod by hand.
struct ContentView: View {
    let link: PodLink
    let navigator: Navigator

    // Test destination: HSR Layout BDA Complex. Paste any "lat, lon" from Apple or Google Maps.
    @State private var destinationText = "12.97160, 77.59460"
    @State private var simulate = true
    @State private var speedUp: UInt64 = 4

    @State private var dir: TurnDir = .right
    @State private var metres: Double = 400
    @State private var road = "MG Road"
    @State private var approach: Task<Void, Never>?

    private var connected: Bool { link.state == .connected }
    private var navigating: Bool { navigator.phase == .navigating || navigator.phase == .routing }

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

                navigationSection

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
                .disabled(!connected || navigating)
            }
            .navigationTitle("BikePod")
        }
    }

    @ViewBuilder
    private var navigationSection: some View {
        Section {
            TextField("Destination (lat, lon)", text: $destinationText)
                .keyboardType(.numbersAndPunctuation)
                .font(.body.monospaced())
            Toggle("Simulate the ride", isOn: $simulate)
            if simulate {
                Picker("Speed", selection: $speedUp) {
                    Text("1×").tag(UInt64(1))
                    Text("4×").tag(UInt64(4))
                    Text("10×").tag(UInt64(10))
                }
                .pickerStyle(.segmented)
            }

            if navigating {
                Button("Stop navigation", role: .destructive) { navigator.stop() }
            } else {
                Button("Start navigation") { startNavigation() }
                    .disabled(destination == nil || !navigator.hasGPS)
            }

            switch navigator.phase {
            case .idle:
                if !navigator.hasGPS { Text("Waiting for GPS…").foregroundStyle(.secondary) }
            case .routing:
                Text("Getting route…").foregroundStyle(.secondary)
            case .navigating:
                LabeledContent("Next", value: navigator.instruction)
                LabeledContent("In", value: "\(navigator.distanceToTurn) m")
                LabeledContent("Remaining", value: String(format: "%.1f km", navigator.remainingKm))
                if navigator.rerouting { Text("Rerouting…").foregroundStyle(.orange) }
            case .arrived:
                Text("Arrived").foregroundStyle(.green)
            case let .failed(message):
                Text(message).foregroundStyle(.red)
            }
        } header: {
            Text("Navigate")
        } footer: {
            Text("Default destination: HSR Layout BDA Complex. Routes by Valhalla, map data © OpenStreetMap contributors.")
        }
    }

    /// Parses "12.97160, 77.59460" (also accepts a space instead of the comma).
    private var destination: CLLocationCoordinate2D? {
        let parts = destinationText
            .split(whereSeparator: { $0 == "," || $0 == " " })
            .compactMap { Double($0) }
        guard parts.count == 2 else { return nil }
        let coordinate = CLLocationCoordinate2D(latitude: parts[0], longitude: parts[1])
        return CLLocationCoordinate2DIsValid(coordinate) ? coordinate : nil
    }

    private func startNavigation() {
        guard let destination else { return }
        stopApproach()
        Task { await navigator.start(to: destination, simulate: simulate, speedUp: speedUp) }
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
