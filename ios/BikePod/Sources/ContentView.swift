import CoreLocation
import FerrostarCore
import FerrostarCoreFFI
import FerrostarMapLibreUI
import MapLibre
import MapLibreSwiftDSL
import MapLibreSwiftUI
import SwiftUI

/// Main screen: search a place, preview the route on the map, then navigate.
/// The phone shows Ferrostar's navigation map; the pod shows the glanceable version.
struct ContentView: View {
    let link: PodLink
    let navigator: Navigator

    /// OpenFreeMap: free OpenStreetMap vector tiles, no API key needed. Dark style in dark mode
    /// (easier on the eyes at night, and matches the pod).
    private static let lightStyle = URL(string: "https://tiles.openfreemap.org/styles/liberty")!
    private static let darkStyle = URL(string: "https://tiles.openfreemap.org/styles/dark")!

    @Environment(\.colorScheme) private var colorScheme
    @State private var search = PlaceSearch()
    @State private var recents = RecentPlaces()
    @State private var saved = SavedPlaces()
    @State private var start: Place?              // nil: start from my location
    @State private var destination: Place?
    @State private var pickingStart = false        // the search box is choosing the starting point
    @State private var savingSlot: SavedPlaces.Slot?   // the search box is choosing Home or Work
    @State private var query = ""
    @State private var camera: MapViewCamera = .trackUserLocation(zoom: 14)
    @State private var showPodTest = false
    @FocusState private var searchFocused: Bool

    private var isNavigating: Bool { navigator.phase == .navigating }
    private var mapStyle: URL { colorScheme == .dark ? Self.darkStyle : Self.lightStyle }
    private var routeAnchor: LayerReferenceBelow { RouteLayer.anchor(dark: colorScheme == .dark) }

    var body: some View {
        DynamicallyOrientingNavigationView(
            styleURL: mapStyle,
            camera: $camera,
            navigationState: navigator.navState,
            isMuted: navigator.isMuted,
            onTapMute: { navigator.toggleMute() },
            onTapExit: { stopNavigation() }
        ) {
            if case let .preview(route) = navigator.phase {
                RouteLayer(polyline: MLNPolylineFeature(coordinates: route.geometry.map(\.clLocationCoordinate2D)),
                           identifier: "preview-route", color: .systemBlue, casingColor: .white, below: routeAnchor)
            }
        }
        .navigationMapViewRouteOverlay { [routeAnchor] state in
            // (Ferrostar's own overlay draws the whole route twice, grey then blue: one blue line is the same)
            if let geometry = state?.routeGeometry, geometry.count >= 2 {
                RouteLayer(polyline: MLNPolylineFeature(coordinates: geometry.map(\.clLocationCoordinate2D)),
                           identifier: "route", color: .systemBlue, casingColor: .white, below: routeAnchor)
            }
        }
        .id(mapStyle)                                   // a new style needs a fresh map
        .overlay(alignment: .top) {
            if !isNavigating { topPanel.padding(.horizontal) }
        }
        .overlay(alignment: .bottom) {
            // The pod's status sits bottom-left on every screen (above the card when there is one)
            if !isNavigating {
                VStack(alignment: .leading, spacing: 10) {
                    if !searchFocused { podButton }        // not floating over the keyboard
                    routePanel
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding()
            }
        }
        .overlay(alignment: .bottomLeading) {
            if isNavigating {
                podButton
                    .padding(.leading, 16)
                    .padding(.bottom, 150)            // clear of the current-road label
            }
        }
        .sheet(isPresented: $showPodTest) {
            PodTestView(link: link, navigator: navigator)
                .onAppear { navigator.podFeedPaused = true }
                .onDisappear { navigator.podFeedPaused = false }
        }
        .onChange(of: navigator.phase) { _, phase in
            if case let .preview(route) = phase {
                camera = .boundingBox(route.mapBounds,
                                      edgePadding: UIEdgeInsets(top: 190, left: 40, bottom: 300, right: 40))
            }
        }
        // A tap you can feel: starting, rerouting, arriving, failing
        .sensoryFeedback(trigger: navigator.phase) { old, new in
            switch new {
            case .navigating where old != .navigating: .start
            case .arrived where old != .arrived: .success
            case .failed: .error
            default: nil
            }
        }
        .sensoryFeedback(.warning, trigger: navigator.rerouting) { _, rerouting in rerouting }
        .task { navigator.activate() }                  // GPS after the first screen is up
    }

    // MARK: Top: search, saved places, pod

    private var topPanel: some View {
        VStack(spacing: 10) {
            searchCard
            if searchFocused, !search.results.isEmpty {
                placeList(search.results, title: nil)
            } else if searchFocused, query.isEmpty {
                placeList(recents.places, title: recents.places.isEmpty ? nil : "Recent")
            } else if navigator.phase == .idle {
                HStack(spacing: 8) {
                    ForEach(SavedPlaces.Slot.allCases) { slot in slotChip(slot) }
                    Spacer()
                }
            }
            if let error = search.error {
                Text(error).font(.caption).foregroundStyle(.red)
            }
        }
        .onChange(of: searchFocused) { _, focused in
            if !focused, pickingStart || savingSlot != nil {   // tapped away: back to "Where to?"
                pickingStart = false
                savingSlot = nil
                query = destination?.name ?? ""
            }
        }
        .onChange(of: query) { _, text in
            search.search(text, near: start?.coordinate ?? navigator.currentCoordinate)
        }
    }

    /// One card: where from (tap to change), then where to
    private var searchCard: some View {
        VStack(spacing: 0) {
            if savingSlot == nil {
                Button {
                    pickingStart = true
                    query = ""
                    searchFocused = true
                } label: {
                    HStack(spacing: 10) {
                        Image(systemName: start == nil ? "location.fill" : "mappin.and.ellipse")
                            .foregroundStyle(.blue)
                            .frame(width: 22)
                        Text(start?.name ?? "My location").lineLimit(1)
                        Spacer()
                    }
                    .font(.subheadline)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .overlay(alignment: .trailing) {
                    if start != nil {
                        Button { setStart(nil) } label: {
                            Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary)
                        }
                        .accessibilityLabel("Start from my location")
                    }
                }
                .padding(.horizontal, 14)
                .padding(.vertical, 11)
                Divider().padding(.leading, 46)
            }
            HStack(spacing: 10) {
                Image(systemName: savingSlot?.symbol ?? (pickingStart ? "location.circle" : "magnifyingglass"))
                    .foregroundStyle(.secondary)
                    .frame(width: 22)
                TextField(searchPrompt, text: $query)
                    .focused($searchFocused)
                    .submitLabel(.search)
                    .autocorrectionDisabled()
                if search.isSearching { ProgressView() }
                if !query.isEmpty {
                    Button { clearSearch() } label: {
                        Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary)
                    }
                    .accessibilityLabel("Clear")
                }
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 12)
        }
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16))
    }

    private var searchPrompt: String {
        if let savingSlot { return "Search for \(savingSlot.rawValue.lowercased())" }
        return pickingStart ? "Starting point" : "Where to?"
    }

    private var podButton: some View {
        Button { showPodTest = true } label: { PodPill(link: link) }
            .buttonStyle(.plain)
            .accessibilityHint("Opens pod status and testing")
    }

    /// Home / Work: goes there, or asks for the place the first time. Long-press to change.
    private func slotChip(_ slot: SavedPlaces.Slot) -> some View {
        let place = saved.places[slot]
        return Button {
            if let place { go(to: place) } else { startSaving(slot) }
        } label: {
            Label(place == nil ? "Set \(slot.rawValue.lowercased())" : slot.rawValue, systemImage: slot.symbol)
                .font(.footnote.weight(.semibold))
                .foregroundStyle(place == nil ? .secondary : .primary)
                .padding(.horizontal, 12)
                .padding(.vertical, 7)
                .background(.regularMaterial, in: Capsule())
        }
        .buttonStyle(.plain)
        .contextMenu {
            if place != nil {
                Button("Change \(slot.rawValue.lowercased())", systemImage: "pencil") { startSaving(slot) }
                Button("Remove", systemImage: "trash", role: .destructive) { saved.set(slot, to: nil) }
            }
        }
    }

    private func placeList(_ places: [Place], title: String?) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                if pickingStart {
                    Button { setStart(nil) } label: {
                        Label("My location", systemImage: "location.fill")
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(.vertical, 12)
                            .padding(.horizontal, 14)
                    }
                    Divider()
                }
                if let title {
                    Text(title).font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                        .padding(.horizontal, 14).padding(.top, 10)
                }
                ForEach(places) { place in
                    placeRow(place, removable: title != nil)
                    Divider().padding(.leading, 50)
                }
            }
        }
        .scrollBounceBehavior(.basedOnSize)
        .frame(maxHeight: 340)
        .fixedSize(horizontal: false, vertical: true)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16))
    }

    private func placeRow(_ place: Place, removable: Bool) -> some View {
        HStack(spacing: 12) {
            Image(systemName: removable ? "clock.arrow.circlepath" : place.symbol)
                .font(.title3)
                .foregroundStyle(.secondary)
                .frame(width: 24)
            Button { choose(place) } label: {
                VStack(alignment: .leading, spacing: 2) {
                    Text(place.name).foregroundStyle(.primary).lineLimit(1)
                    let detail = [distanceText(to: place), place.detail.isEmpty ? nil : place.detail]
                        .compactMap { $0 }.joined(separator: " · ")
                    if !detail.isEmpty {
                        Text(detail).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if removable {                                  // recents can be removed one by one
                Button { recents.forget(place) } label: {
                    Image(systemName: "xmark").font(.caption).foregroundStyle(.tertiary)
                }
                .accessibilityLabel("Remove \(place.name)")
            }
        }
        .padding(.vertical, 10)
        .padding(.horizontal, 14)
        .contextMenu {
            ForEach(SavedPlaces.Slot.allCases) { slot in
                Button("Set as \(slot.rawValue.lowercased())", systemImage: slot.symbol) { saved.set(slot, to: place) }
            }
        }
    }

    /// "2.3 km" from where you are (or from the chosen start)
    private func distanceText(to place: Place) -> String? {
        guard let from = start?.coordinate ?? navigator.currentCoordinate else { return nil }
        let metres = CLLocation(latitude: from.latitude, longitude: from.longitude)
            .distance(from: CLLocation(latitude: place.latitude, longitude: place.longitude))
        if metres < 1000 { return "\(Int((metres / 10).rounded()) * 10) m" }
        if metres < 100_000 { return String(format: "%.1f km", metres / 1000) }
        return "\((metres / 1000).rounded().formatted(.number.precision(.fractionLength(0)))) km"
    }

    // MARK: Bottom: route preview, status, arrival

    @ViewBuilder
    private var routePanel: some View {
        switch navigator.phase {
        case .idle:
            if !navigator.hasGPS {
                card { Label("Waiting for GPS…", systemImage: "location.slash") }
            }
        case .routing:
            card { HStack(spacing: 10) { ProgressView(); Text("Finding a route…") } }
        case let .preview(route):
            previewCard(route)
        case .navigating:
            EmptyView()
        case .arrived:
            arrivedCard
        case let .failed(message):
            card {
                VStack(alignment: .leading, spacing: 14) {
                    HStack(alignment: .top, spacing: 10) {
                        Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                        Text(message)
                    }
                    HStack {
                        Button("Close") { cancelRoute() }
                            .buttonStyle(.bordered)
                        if destination != nil {
                            Button { planRoute() } label: {
                                Label("Try again", systemImage: "arrow.clockwise").frame(maxWidth: .infinity)
                            }
                            .buttonStyle(.borderedProminent)
                        }
                    }
                    .controlSize(.large)
                }
            }
        }
    }

    private func previewCard(_ route: Route) -> some View {
        card {
            VStack(alignment: .leading, spacing: 14) {
                HStack(alignment: .top) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(navigator.destinationName).font(.headline).lineLimit(2)
                        TimelineView(.everyMinute) { context in
                            Text("Arrive \(context.date.addingTimeInterval(route.durationSeconds), format: .dateTime.hour().minute())")
                                .font(.system(size: 30, weight: .bold, design: .rounded))
                        }
                        Text("\(route.distanceText) · \(route.durationText)").foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button { cancelRoute() } label: {
                        Image(systemName: "xmark")
                            .font(.body.weight(.semibold))
                            .foregroundStyle(.secondary)
                            .frame(width: 32, height: 32)
                            .background(.thinMaterial, in: Circle())
                    }
                    .accessibilityLabel("Cancel route")
                }
                if navigator.willSimulate {
                    Label(navigator.routeStartsElsewhere
                          ? "Starts away from you, so the ride will be simulated"
                          : "Ride simulation is on (\(navigator.simulationSpeed)×)",
                          systemImage: "play.circle")
                        .font(.footnote)
                        .foregroundStyle(.orange)
                }
                Button { startNavigation() } label: {
                    Label(navigator.willSimulate ? "Start simulation" : "Start", systemImage: "location.north.fill")
                        .font(.headline)
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                Text("Routes by Valhalla · Map data © OpenStreetMap contributors")
                    .font(.caption2).foregroundStyle(.tertiary)
            }
        }
    }

    private var arrivedCard: some View {
        card {
            VStack(alignment: .leading, spacing: 16) {
                HStack(spacing: 12) {
                    Image(systemName: "flag.checkered.circle.fill").font(.largeTitle).foregroundStyle(.red)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Arrived").font(.title3.bold())
                        Text(navigator.destinationName).foregroundStyle(.secondary).lineLimit(1)
                    }
                }
                if let trip = navigator.lastTrip {
                    HStack(spacing: 0) {
                        stat("Distance", String(format: "%.1f km", trip.km))
                        stat("Time", "\(max(1, Int((trip.seconds / 60).rounded()))) min")
                        stat("Avg speed", trip.seconds > 30 ? "\(Int((trip.km / (trip.seconds / 3600)).rounded())) km/h" : "–")
                    }
                }
                Button { stopNavigation() } label: {
                    Text("Done").font(.headline).frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
            }
        }
    }

    private func stat(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(value).font(.title3.weight(.semibold)).monospacedDigit()
            Text(title).font(.caption).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func card(@ViewBuilder _ content: () -> some View) -> some View {
        content()
            .padding()
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 22))
    }

    // MARK: Actions

    private func choose(_ place: Place) {
        search.clear()
        if let savingSlot {
            saved.set(savingSlot, to: place)
            self.savingSlot = nil
            searchFocused = false
            query = destination?.name ?? ""
            return
        }
        recents.remember(place)
        if pickingStart {
            setStart(place)
            return
        }
        go(to: place)
    }

    private func go(to place: Place) {
        searchFocused = false
        query = place.name
        destination = place
        planRoute()
    }

    private func startSaving(_ slot: SavedPlaces.Slot) {
        pickingStart = false
        savingSlot = slot
        query = ""
        searchFocused = true
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

    private func cancelRoute() {
        navigator.stop()
        destination = nil
        query = ""
        camera = .trackUserLocation(zoom: 14)
    }

    private func clearSearch() {
        query = ""
        search.clear()
        if pickingStart { pickingStart = false; query = destination?.name ?? "" }
    }

    private func startNavigation() {
        navigator.startNavigation()
        guard navigator.phase == .navigating else { return }
        camera = .automotiveNavigation()
        UIApplication.shared.isIdleTimerDisabled = true   // keep the phone screen on while navigating
    }

    private func stopNavigation() {
        navigator.stop()
        destination = nil
        query = ""
        camera = .trackUserLocation(zoom: 14)
        UIApplication.shared.isIdleTimerDisabled = false
    }
}

private extension Route {
    var mapBounds: MLNCoordinateBounds {
        MLNCoordinateBounds(sw: bbox.sw.clLocationCoordinate2D, ne: bbox.ne.clLocationCoordinate2D)
    }

    var durationSeconds: TimeInterval { steps.reduce(0) { $0 + $1.duration } }

    var distanceText: String {
        distance >= 1000 ? String(format: "%.1f km", distance / 1000) : "\(Int(distance)) m"
    }

    var durationText: String {
        let minutes = Int((durationSeconds / 60).rounded())
        return minutes >= 60 ? "\(minutes / 60) h \(minutes % 60) min" : "\(minutes) min"
    }
}
