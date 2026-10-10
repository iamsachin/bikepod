import MapLibre
import MapLibreSwiftDSL
import UIKit

/// The route line on the phone's map: a coloured line with an optional casing, Ferrostar's look,
/// but placed below a layer we choose.
///
/// Ferrostar puts its route below the style's first label layer. In OpenFreeMap's dark style that
/// layer (water names) comes before every road, so the roads were drawn over the route and only its
/// white casing showed. In the dark style it now goes over the road names too (below borders and
/// place names): those are grey with a black outline, which on the blue route read as black text.
/// (The road being ridden is named in the navigation view's own label.)
struct RouteLayer: StyleLayerCollection {
    let polyline: MLNPolyline
    let identifier: String
    let color: UIColor
    let casingColor: UIColor?
    let below: LayerReferenceBelow

    /// Where the route goes in each map style
    static func anchor(dark: Bool) -> LayerReferenceBelow {
        dark ? .layer(layerId: "boundary_state") : .symbols
    }

    var layers: [StyleLayerDefinition] {
        let source = ShapeSource(identifier: "\(identifier)-source") {
            polyline
        }

        if let casingColor {
            LineStyleLayer(identifier: "\(identifier)-casing", source: source)
                .lineCap(.round)
                .lineJoin(.round)
                .lineColor(casingColor)
                .lineWidth(interpolatedBy: .zoomLevel,
                           curveType: .exponential,
                           parameters: NSExpression(forConstantValue: 1.5),
                           stops: NSExpression(forConstantValue: [14: 6, 18: 24]))
                .renderBelow(below)
        }

        LineStyleLayer(identifier: "\(identifier)-polyline", source: source)
            .lineCap(.round)
            .lineJoin(.round)
            .lineColor(color)
            .lineWidth(interpolatedBy: .zoomLevel,
                       curveType: .exponential,
                       parameters: NSExpression(forConstantValue: 1.5),
                       stops: NSExpression(forConstantValue: [14: 3, 18: 16]))
            .renderBelow(below)
    }
}
