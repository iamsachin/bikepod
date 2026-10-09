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
    @State private var recents = RecentPlaces()
    @State private var start: Place?              // nil: start from my location
    @State private var destination: Place?
    @State private var pickingStart = false        // the search box is choosing the starting point
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
        .sheet(isPresented: $showPodTest) {
            PodTestView(link: link)
                .onAppear { navigator.podFeedPaused = true }
                .onDisappear { navigator.podFeedPaused = false }
        }
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
            startRow
            HStack {
                Image(systemName: pickingStart ? "location.circle" : "magnifyingglass").foregroundStyle(.secondary)
                TextField(pickingStart ? "Starting point" : "Where to?", text: $query)
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
                placeList(search.results, icon: "mappin.circle")
            } else if searchFocused, query.isEmpty, !recents.places.isEmpty || pickingStart {
                placeList(recents.places, icon: "clock.arrow.circlepath", title: "Recent")
            }
            if let error = search.error {
                Text(error).font(.caption).foregroundStyle(.red)
            }
        }
        .onChange(of: searchFocused) { _, focused in
            if !focused, pickingStart {          // tapped away while choosing a start: back to "Where to?"
                pickingStart = false
                query = destination?.name ?? ""
            }
        }
        .onChange(of: query) { _, text in
            search.search(text, near: start?.coordinate ?? navigator.currentCoordinate)
        }
    }

    /// "From: My location" / "From: Agara Lake ✕". Tapping it makes the search box pick the start.
    private var startRow: some View {
        HStack(spacing: 8) {
            Button {
                pickingStart = true
                query = ""
                searchFocused = true
            } label: {
                HStack(spacing: 6) {
                    Image(systemName: start == nil ? "location.fill" : "mappin.and.ellipse")
                    Text("From: ").foregroundStyle(.secondary) + Text(start?.name ?? "My location")
                }
                .font(.subheadline)
                .lineLimit(1)
            }
            .buttonStyle(.plain)
            if start != nil {
                Button { setStart(nil) } label: {
                    Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary)
                }
                .accessibilityLabel("Start from my location")
            }
            Spacer()
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
    }

    private func placeList(_ places: [Place], icon: String, title: String? = nil) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                if pickingStart {
                    Button { setStart(nil) } label: {
                        Label("My location", systemImage: "location.fill")
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(.vertical, 10)
                            .padding(.horizontal, 12)
                    }
                    Divider()
                }
                if let title {
                    Text(title).font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                        .padding(.horizontal, 12).padding(.top, 10)
                }
                ForEach(places) { place in
                    HStack(spacing: 10) {
                        Image(systemName: icon).foregroundStyle(.secondary)
                        Button { choose(place) } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(place.name).foregroundStyle(.primary)
                                if !place.detail.isEmpty {
                                    Text(place.detail).font(.caption).foregroundStyle(.secondary)
                                }
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        if title != nil {   // recents can be removed one by one
                            Button { recents.forget(place) } label: {
                                Image(systemName: "xmark").font(.caption).foregroundStyle(.tertiary)
                            }
                            .accessibilityLabel("Remove \(place.name)")
                        }
                    }
                    .padding(.vertical, 10)
                    .padding(.horizontal, 12)
                    Divider()
                }
            }
        }
        .frame(maxHeight: 320)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
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
                    if navigator.routeStartsElsewhere {
                        Label("Starts away from you, so the ride is simulated", systemImage: "info.circle")
                            .font(.footnote).foregroundStyle(.secondary)
                    } else {
                        Toggle("Simulate the ride", isOn: $simulate)
                    }
                    if simulate || navigator.routeStartsElsewhere {
                        Picker("Speed", selection: $speedUp) {
                            Text("1×").tag(UInt64(1))
                            Text("4×").tag(UInt64(4))
                            Text("10×").tag(UInt64(10))
                        }
                        .pickerStyle(.segmented)
                    }
                    HStack {
                        Button("Cancel") {
                            navigator.stop()
                            destination = nil
                            camera = .trackUserLocation(zoom: 14)
                        }
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
        recents.remember(place)
        search.clear()
        if pickingStart {
            setStart(place)
            return
        }
        searchFocused = false
        query = place.name
        destination = place
        planRoute()
    }

    private func setStart(_ place: Place?) {
        start = place
        pickingStart = false
        searchFocused = false
        query = destination?.name ?? ""
        search.clear()
        if destination != nil { planRoute() }
    }

    private func planRoute() {
        guard let destination else { return }
        let from = start?.coordinate
        Task { await navigator.planRoute(from: from, to: destination.coordinate, name: destination.name) }
    }

    private func clearSearch() {
        query = ""
        search.clear()
        if pickingStart { pickingStart = false; query = destination?.name ?? "" }
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
