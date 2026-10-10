import SwiftUI

@main
struct BikePodApp: App {
    @State private var link: PodLink
    @State private var navigator: Navigator
    @State private var albumArt: AlbumArt

    init() {
        let link = PodLink()
        _link = State(initialValue: link)
        _navigator = State(initialValue: Navigator(link: link))
        _albumArt = State(initialValue: AlbumArt(link: link))      // covers for the pod's music page
    }

    var body: some Scene {
        WindowGroup {
            ContentView(link: link, navigator: navigator)
        }
    }
}
