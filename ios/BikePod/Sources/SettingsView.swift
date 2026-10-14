import SwiftUI

/// Settings (from the BikePod pill): pod status, navigation, weather and ride simulation
struct SettingsView: View {
    let link: PodLink
    @Bindable var navigator: Navigator
    @Environment(\.dismiss) private var dismiss

    /// Optional Open-Meteo API key (Keychain); empty = the free service
    @State private var weatherKey = Keychain.string(Weather.keyName) ?? ""

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
                    Toggle("Voice guidance", isOn: Binding(get: { !navigator.isMuted },
                                                           set: { navigator.setMuted(!$0) }))
                    Toggle("Lock screen & Dynamic Island", isOn: $navigator.liveActivityEnabled)
                } header: {
                    Text("Navigation")
                } footer: {
                    Text("Voice: spoken turn instructions from the phone (also on the speaker button while navigating). Lock screen & Dynamic Island: the next turn while the phone is locked or in another app; off saves battery on long rides.")
                }
                Section {
                    SecureField("API key (optional)", text: $weatherKey)
                        .textContentType(.password)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                        .onChange(of: weatherKey) { _, key in
                            Keychain.set(key.trimmingCharacters(in: .whitespacesAndNewlines), for: Weather.keyName)
                        }
                } header: {
                    Text("Weather")
                } footer: {
                    Text("Rain ahead on the pod's journey page comes from Open-Meteo, free without a key for personal use. With a key from an Open-Meteo plan, its customer service is used. The key is kept in the iPhone's Keychain.")
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
                    Text("Replays the route with fake GPS, for testing at a desk. On at every start of development builds; off in release builds.")
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                Button("Done") { dismiss() }
            }
        }
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

