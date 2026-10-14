import Foundation

/// Simulator only: every line the app would send the pod, with the time, in Documents/podfeed.log.
/// The Simulator has no Bluetooth; replayed into the pod over USB, a simulated ride can be measured
/// on the real hardware (frame times while the map moves).
enum PodFeedRecorder {
    #if targetEnvironment(simulator)
    private static let queue = DispatchQueue(label: "podfeed")
    private static let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("podfeed.log")
    private static var start: Date?
    private static var lastArea: String?
    #endif

    static func record(_ lines: [String]) {
        #if targetEnvironment(simulator)
        let now = Date()
        queue.async {
            if start == nil {
                start = now
                try? FileManager.default.removeItem(at: url)
                FileManager.default.createFile(atPath: url.path, contents: nil)
            }
            let t = now.timeIntervalSince(start!)
            let text = lines.map { String(format: "%.3f\t%@\n", t, $0) }.joined()
            if let handle = try? FileHandle(forWritingTo: url) {
                handle.seekToEndOfFile()
                handle.write(Data(text.utf8))
                try? handle.close()
            }
        }
        #endif
    }

    /// A map area, once (the app offers it again on every update while no pod is connected)
    static func recordArea(_ lines: [String]) {
        #if targetEnvironment(simulator)
        guard let first = lines.first, first != lastArea else { return }
        lastArea = first
        record(lines)
        #endif
    }
}
