import CoreLocation
import FerrostarCore
import FerrostarCoreFFI
import FerrostarMapLibreUI
import MapLibre
import MapLibreSwiftDSL
import MapLibreSwiftUI
import SwiftUI

/// Step 5 main screen: search a place, preview the route on the map, then navigate.
/// The phone shows Ferrostar's navigation map; the pod shows the glanceable version.
struct ContentView: View {
    let link: PodLink
    let navigator: Navigator

    /// OpenFreeMap: free OpenStreetMap vector tiles, no API key needed
    private static let mapStyle = URL(string: "https://tiles.openfreemap.org/styles/liberty")!

    @State private var search = PlaceSearch()
    @State private var query = ""
    @State private var camera: MapViewCamera = .trackUserLocation(zoom: 14)
    @State private var simulate = true
    @State private var speedUp: UInt64 = 4
    @State private var showPodTest = false
    @FocusState private var searchFocused: Bool

    private var isNavigating: Bool { navigator.phase == .navigating }

    var body: some View {
        DynamicallyOrientingNavigationView(
            styleURL: Self.mapStyle,
            camera: $camera,
            navigationState: navigator.navState,
            isMuted: navigator.isMuted,
            onTapMute: { navigator.toggleMute() },
            onTapExit: { stopNavigation() }
        ) {
            if case let .preview(route) = navigator.phase {
                RouteStyleLayer(polyline: MLNPolylineFeature(coordinates: route.geometry.map(\.clLocationCoordinate2D)),
                                identifier: "preview-route")
            }
        }
        .overlay(alignment: .top) {
            if !isNavigating { searchPanel.padding(.horizontal) }
        }
        .overlay(alignment: .bottom) {
            if !isNavigating { routePanel.padding() }
        }
        .overlay(alignment: .bottomLeading) {
            if isNavigating {
                PodStatusLabel(link: link)
                    .font(.caption)
                    .padding(8)
                    .background(.regularMaterial, in: Capsule())
                    .padding(.leading, 16)
                    .padding(.bottom, 110)
            }
        }
        .sheet(isPresented: $showPodTest) { PodTestView(link: link) }
        .onChange(of: navigator.phase) { _, phase in
            if case let .preview(route) = phase {
                camera = .boundingBox(route.mapBounds,
                                      edgePadding: UIEdgeInsets(top: 140, left: 40, bottom: 260, right: 40))
            }
        }
    }

    // MARK: Search

    private var searchPanel: some View {
        VStack(spacing: 8) {
            HStack {
                Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                TextField("Where to?", text: $query)
                    .focused($searchFocused)
                    .submitLabel(.search)
                    .autocorrectionDisabled()
                if search.isSearching { ProgressView() }
                if !query.isEmpty {
                    Button { clearSearch() } label: {
                        Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary)
                    }
                }
                Divider().frame(height: 20)
                Button { showPodTest = true } label: {
                    Image(systemName: "dot.radiowaves.left.and.right")
                        .foregroundStyle(link.state == .connected ? .green : .orange)
                }
                .accessibilityLabel("Pod test")
            }
            .padding(12)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))

            if searchFocused, !search.results.isEmpty {
                ScrollView {
                    VStack(alignment: .leading, spacing: 0) {
                        ForEach(search.results) { place in
                            Button { choose(place) } label: {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(place.name).foregroundStyle(.primary)
                                    if !place.detail.isEmpty {
                                        Text(place.detail).font(.caption).foregroundStyle(.secondary)
                                    }
                                }
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .padding(.vertical, 10)
                                .padding(.horizontal, 12)
                            }
                            Divider()
                        }
                    }
                }
                .frame(maxHeight: 320)
                .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
            }
            if let error = search.error {
                Text(error).font(.caption).foregroundStyle(.red)
            }
        }
        .onChange(of: query) { _, text in
            search.search(text, near: navigator.currentCoordinate)
        }
    }

    // MARK: Route preview / status

    @ViewBuilder
    private var routePanel: some View {
        switch navigator.phase {
        case .idle:
            if !navigator.hasGPS {
                panel { Label("Waiting for GPS…", systemImage: "location.slash") }
            }
        case .routing:
            panel { HStack { ProgressView(); Text("Finding a route…") } }
        case let .preview(route):
            panel {
                VStack(alignment: .leading, spacing: 12) {
                    Text(navigator.destinationName).font(.headline).lineLimit(2)
                    Text("\(route.distanceText) · \(route.durationText) by motorcycle")
                        .foregroundStyle(.secondary)
                    Toggle("Simulate the ride", isOn: $simulate)
                    if simulate {
                        Picker("Speed", selection: $speedUp) {
                            Text("1×").tag(UInt64(1))
                            Text("4×").tag(UInt64(4))
                            Text("10×").tag(UInt64(10))
                        }
                        .pickerStyle(.segmented)
                    }
                    HStack {
                        Button("Cancel") { navigator.stop(); camera = .trackUserLocation(zoom: 14) }
                            .buttonStyle(.bordered)
                        Button { startNavigation() } label: {
                            Text("Start").frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.borderedProminent)
                    }
                    Text("Routes by Valhalla · Map data © OpenStreetMap contributors")
                        .font(.caption2).foregroundStyle(.secondary)
                }
            }
        case .navigating:
            EmptyView()
        case .arrived:
            panel {
                HStack {
                    Label("Arrived", systemImage: "flag.checkered").font(.headline)
                    Spacer()
                    Button("Done") { stopNavigation() }.buttonStyle(.borderedProminent)
                }
            }
        case let .failed(message):
            panel {
                HStack {
                    Text(message).foregroundStyle(.red)
                    Spacer()
                    Button("OK") { navigator.stop() }
                }
            }
        }
    }

    private func panel(@ViewBuilder _ content: () -> some View) -> some View {
        content()
            .padding()
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 18))
    }

    // MARK: Actions

    private func choose(_ place: Place) {
        searchFocused = false
        query = place.name
        search.clear()
        Task { await navigator.planRoute(to: place.coordinate, name: place.name) }
    }

    private func clearSearch() {
        query = ""
        search.clear()
    }

    private func startNavigation() {
        navigator.startNavigation(simulate: simulate, speedUp: speedUp)
        camera = .automotiveNavigation()
        UIApplication.shared.isIdleTimerDisabled = true   // keep the phone screen on while navigating
    }

    private func stopNavigation() {
        navigator.stop()
        camera = .trackUserLocation(zoom: 14)
        UIApplication.shared.isIdleTimerDisabled = false
    }
}

private extension Route {
    var mapBounds: MLNCoordinateBounds {
        MLNCoordinateBounds(sw: bbox.sw.clLocationCoordinate2D, ne: bbox.ne.clLocationCoordinate2D)
    }

    var distanceText: String {
        distance >= 1000 ? String(format: "%.1f km", distance / 1000) : "\(Int(distance)) m"
    }

    var durationText: String {
        let minutes = Int((steps.reduce(0) { $0 + $1.duration } / 60).rounded())
        return minutes >= 60 ? "\(minutes / 60) h \(minutes % 60) min" : "\(minutes) min"
    }
}
