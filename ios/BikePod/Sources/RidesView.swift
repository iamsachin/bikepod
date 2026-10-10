import CoreLocation
import MapLibre
import MapLibreSwiftDSL
import MapLibreSwiftUI
import SwiftUI

/// Past rides: a list, newest first, and each ride's track and numbers
struct RidesView: View {
    let history: RideHistory
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Group {
                if history.rides.isEmpty {
                    ContentUnavailableView("No rides yet", systemImage: "point.bottomleft.forward.to.point.topright.scurvepath",
                                           description: Text("Rides you navigate with BikePod are kept here."))
                } else {
                    List {
                        Section {
                            ForEach(history.rides) { ride in
                                NavigationLink(value: ride) { RideRow(ride: ride) }
                            }
                            .onDelete { offsets in
                                let doomed = offsets.map { history.rides[$0] }
                                doomed.forEach(history.delete)
                            }
                        } header: {
                            let km = history.rides.reduce(0) { $0 + $1.km }
                            Text("\(km.formatted(.number.precision(.fractionLength(km < 100 ? 1 : 0)))) km in \(history.rides.count) \(history.rides.count == 1 ? "ride" : "rides")")
                        }
                    }
                }
            }
            .navigationTitle("Rides")
            .navigationDestination(for: Ride.self) { ride in RideDetailView(ride: ride, history: history) }
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
        }
    }
}

private struct RideRow: View {
    let ride: Ride

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            VStack(alignment: .leading, spacing: 3) {
                Text(ride.title).font(.headline).lineLimit(1)
                Text(ride.start, format: .dateTime.weekday(.abbreviated).day().month(.abbreviated).hour().minute())
                    .font(.subheadline).foregroundStyle(.secondary)
                if ride.simulated {
                    Text("Simulated").font(.caption.weight(.semibold)).foregroundStyle(.orange)
                }
            }
            Spacer()
            VStack(alignment: .trailing, spacing: 3) {
                Text(ride.kmText).font(.headline).monospacedDigit()
                Text(ride.durationText).font(.subheadline).foregroundStyle(.secondary).monospacedDigit()
            }
        }
        .padding(.vertical, 2)
    }
}

struct RideDetailView: View {
    let ride: Ride
    let history: RideHistory
    @Environment(\.colorScheme) private var colorScheme
    @Environment(\.dismiss) private var dismiss
    @State private var track: [CLLocationCoordinate2D] = []
    @State private var camera: MapViewCamera = .default()
    @State private var confirmDelete = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                map
                    .frame(height: 320)
                    .clipShape(RoundedRectangle(cornerRadius: 18))
                VStack(alignment: .leading, spacing: 4) {
                    Text(ride.start, format: .dateTime.weekday(.wide).day().month(.wide).year())
                        .font(.subheadline).foregroundStyle(.secondary)
                    Text("\(ride.start.formatted(date: .omitted, time: .shortened)) – \(ride.end.formatted(date: .omitted, time: .shortened))")
                        .font(.subheadline).foregroundStyle(.secondary)
                    if !ride.arrived {
                        Text("Ended before arriving").font(.footnote).foregroundStyle(.secondary)
                    }
                    if ride.simulated {
                        Text("Simulated ride").font(.footnote.weight(.semibold)).foregroundStyle(.orange)
                    }
                }
                Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 18) {
                    GridRow {
                        stat("Distance", ride.kmText)
                        stat("Time", ride.durationText)
                    }
                    GridRow {
                        stat("Avg speed", ride.averageKmh.map { "\(Int($0.rounded())) km/h" } ?? "–")
                        stat("Top speed", ride.topSpeedKmh > 0 ? "\(Int(ride.topSpeedKmh.rounded())) km/h" : "–")
                    }
                    GridRow {
                        stat("Max lean left", ride.maxLeanLeft.map { "\($0)°" } ?? "–")
                        stat("Max lean right", ride.maxLeanRight.map { "\($0)°" } ?? "–")
                    }
                }
                if ride.maxLeanLeft == nil {
                    Text("Lean is measured by the pod, so it shows when the pod was connected.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            .padding()
        }
        .navigationTitle(ride.title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            Button(role: .destructive) { confirmDelete = true } label: { Image(systemName: "trash") }
                .accessibilityLabel("Delete ride")
        }
        .confirmationDialog("Delete this ride?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete ride", role: .destructive) {
                history.delete(ride)
                dismiss()
            }
        }
        .task {
            track = history.track(for: ride)
            if let bounds = Self.bounds(track) {
                camera = .boundingBox(bounds, edgePadding: UIEdgeInsets(top: 40, left: 40, bottom: 40, right: 40))
            }
        }
    }

    private var map: some View {
        let style = colorScheme == .dark ? ContentView.darkStyle : ContentView.lightStyle
        return MapView(styleURL: style, camera: $camera) {
            if track.count >= 2 {
                RouteLayer(polyline: MLNPolylineFeature(coordinates: track), identifier: "ride",
                           color: .systemBlue, casingColor: .white, below: RouteLayer.anchor(dark: colorScheme == .dark))
                CircleStyleLayer(identifier: "ride-start",
                                 source: ShapeSource(identifier: "ride-start-source") { MLNPointFeature(coordinate: track[0]) })
                    .radius(7)
                    .color(.systemGreen)
                    .strokeWidth(3)
                    .strokeColor(.white)
                CircleStyleLayer(identifier: "ride-end",
                                 source: ShapeSource(identifier: "ride-end-source") { MLNPointFeature(coordinate: track[track.count - 1]) })
                    .radius(7)
                    .color(.systemRed)
                    .strokeWidth(3)
                    .strokeColor(.white)
            }
        }
        .id(track.count)                     // draw the track once it has loaded
    }

    private func stat(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(value).font(.title2.weight(.semibold)).monospacedDigit()
            Text(title).font(.caption).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private static func bounds(_ track: [CLLocationCoordinate2D]) -> MLNCoordinateBounds? {
        guard let first = track.first else { return nil }
        var sw = first, ne = first
        for point in track {
            sw.latitude = min(sw.latitude, point.latitude)
            sw.longitude = min(sw.longitude, point.longitude)
            ne.latitude = max(ne.latitude, point.latitude)
            ne.longitude = max(ne.longitude, point.longitude)
        }
        return MLNCoordinateBounds(sw: sw, ne: ne)
    }
}

private extension Ride {
    var title: String { destination.isEmpty ? "Ride" : destination }

    var kmText: String {
        km < 10 ? String(format: "%.1f km", km) : "\(Int(km.rounded())) km"
    }

    var durationText: String {
        let minutes = max(1, Int((seconds / 60).rounded()))
        return minutes >= 60 ? "\(minutes / 60) h \(minutes % 60) min" : "\(minutes) min"
    }
}
