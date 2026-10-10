import Foundation
import UIKit

/// Album art for the pod's Now playing page.
///
/// iOS shares the song's title and artist with the pod but not its artwork, so when the pod says
/// what's playing (NOWPLAYING|id|title|artist) the cover is looked up in the iTunes catalogue,
/// shrunk to 120x120 RGB565 and sent back as ART lines (see firmware/pod/BLE_Link.h).
@MainActor
final class AlbumArt {
    private static let size = 120
    private static let partBytes = 360                       // raw bytes per ART line (480 in base64)

    private let link: PodLink
    private var cache: [String: Data] = [:]                  // "title|artist" -> RGB565 pixels

    init(link: PodLink) {
        self.link = link
        link.onEvent { [weak self] line in self?.handle(line) }
    }

    private func handle(_ line: String) {
        let fields = line.split(separator: "|", omittingEmptySubsequences: false).map(String.init)
        guard fields.count >= 4, fields[0] == "NOWPLAYING", let id = Int(fields[1]) else { return }
        let title = fields[2], artist = fields[3], key = "\(title)|\(artist)"
        Task {
            if cache[key] == nil, let pixels = await Self.lookUp(title: title, artist: artist) {
                if cache.count > 30 { cache.removeAll() }
                cache[key] = pixels
            }
            guard let pixels = cache[key] else {
                DebugLog.write("ART no cover for \(title) / \(artist)")
                return
            }
            send(id: id, pixels: pixels)
        }
    }

    private func send(id: Int, pixels: Data) {
        let parts = (pixels.count + Self.partBytes - 1) / Self.partBytes
        for part in 0..<parts {
            let chunk = pixels.subdata(in: part * Self.partBytes..<min((part + 1) * Self.partBytes, pixels.count))
            link.send(line: "ART|\(id)|\(part)|\(parts)|\(chunk.base64EncodedString())", priority: .bulk)
        }
    }

    /// The cover from the iTunes Search API, as 120x120 RGB565 (little-endian), or nil
    private static func lookUp(title: String, artist: String) async -> Data? {
        // Film songs come as "Tum Hi Ho (From "Aashiqui 2")": search for the song's own name and
        // the first artist
        var name = title
        for cut in [" (From", " (from", " [From", " - ", " (feat", " (Feat"] {
            if let range = name.range(of: cut), range.lowerBound != name.startIndex { name = String(name[..<range.lowerBound]) }
        }
        let firstArtist = artist.split(separator: ",").first.map(String.init) ?? artist
        var components = URLComponents(string: "https://itunes.apple.com/search")!
        components.queryItems = [
            URLQueryItem(name: "term", value: "\(name) \(firstArtist)"),
            URLQueryItem(name: "media", value: "music"),
            URLQueryItem(name: "entity", value: "song"),
            URLQueryItem(name: "limit", value: "1"),
            URLQueryItem(name: "country", value: "IN"),
        ]
        struct Response: Decodable {
            struct Result: Decodable { let artworkUrl100: String? }
            let results: [Result]
        }
        guard let url = components.url,
              let (data, _) = try? await URLSession.shared.data(from: url),
              let response = try? JSONDecoder().decode(Response.self, from: data),
              let small = response.results.first?.artworkUrl100,
              let artURL = URL(string: small.replacingOccurrences(of: "100x100", with: "240x240")),
              let (imageData, _) = try? await URLSession.shared.data(from: artURL),
              let image = UIImage(data: imageData)?.cgImage
        else { return nil }
        return rgb565(image)
    }

    private static func rgb565(_ image: CGImage) -> Data? {
        let n = size
        var rgba = [UInt8](repeating: 0, count: n * n * 4)
        guard let context = CGContext(data: &rgba, width: n, height: n, bitsPerComponent: 8, bytesPerRow: n * 4,
                                      space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { return nil }
        context.interpolationQuality = .high
        context.draw(image, in: CGRect(x: 0, y: 0, width: n, height: n))
        var out = Data(capacity: n * n * 2)
        for i in 0..<(n * n) {
            let r = UInt16(rgba[i * 4]), g = UInt16(rgba[i * 4 + 1]), b = UInt16(rgba[i * 4 + 2])
            let c = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)
            out.append(UInt8(c & 0xFF))
            out.append(UInt8(c >> 8))
        }
        return out
    }
}
