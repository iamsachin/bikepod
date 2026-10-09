import SwiftUI

@main
struct BikePodApp: App {
    @State private var link = PodLink()

    var body: some Scene {
        WindowGroup {
            ContentView(link: link)
        }
    }
}
