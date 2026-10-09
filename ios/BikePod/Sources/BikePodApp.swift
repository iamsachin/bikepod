import SwiftUI

@main
struct BikePodApp: App {
    @State private var link: PodLink
    @State private var navigator: Navigator

    init() {
        let link = PodLink()
        _link = State(initialValue: link)
        _navigator = State(initialValue: Navigator(link: link))
    }

    var body: some Scene {
        WindowGroup {
            ContentView(link: link, navigator: navigator)
        }
    }
}
