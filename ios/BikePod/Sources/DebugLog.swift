import Foundation

/// A small text log in the app's Documents folder, for chasing problems seen on rides.
/// Copy it to the Mac with:
///   xcrun devicectl device copy from --device <id> --domain-type appDataContainer
///     --domain-identifier com.sachinrajput.bikepod --source Documents/debug.log --destination debug.log
enum DebugLog {
    private static let queue = DispatchQueue(label: "debuglog")
    private static let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("debug.log")
    private static let maxBytes = 400_000
    private static let stamp: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SSS"
        return f
    }()

    static func write(_ message: String) {
        let line = "\(stamp.string(from: Date())) \(message)\n"
        queue.async {
            if let size = try? FileManager.default.attributesOfItem(atPath: url.path)[.size] as? Int, size > maxBytes {
                try? FileManager.default.removeItem(at: url)            // start over rather than grow
            }
            if let handle = try? FileHandle(forWritingTo: url) {
                handle.seekToEndOfFile()
                handle.write(Data(line.utf8))
                try? handle.close()
            } else {
                try? Data(line.utf8).write(to: url)
            }
        }
    }
}
