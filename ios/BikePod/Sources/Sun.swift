import Foundation

/// The sun's height above the horizon, for dimming the pod at night. The usual approximation
/// (declination and equation of time from the day of the year): within about a degree, plenty
/// for telling day from night.
enum Sun {
    /// Degrees above the horizon (negative: below) at a place and moment
    static func elevation(lat: Double, lng: Double, at date: Date = Date()) -> Double {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(identifier: "UTC")!
        let day = Double(calendar.ordinality(of: .day, in: .year, for: date) ?? 1)
        let parts = calendar.dateComponents([.hour, .minute, .second], from: date)
        let utcMinutes = Double(parts.hour ?? 0) * 60 + Double(parts.minute ?? 0) + Double(parts.second ?? 0) / 60

        let rad = Double.pi / 180
        let declination = 23.44 * sin(2 * .pi * (284 + day) / 365) * rad
        let b = 2 * Double.pi * (day - 81) / 364
        let equationOfTime = 9.87 * sin(2 * b) - 7.53 * cos(b) - 1.5 * sin(b)       // minutes
        let solarMinutes = utcMinutes + 4 * lng + equationOfTime
        let hourAngle = (solarMinutes / 4 - 180) * rad
        let latitude = lat * rad
        let sinElevation = sin(latitude) * sin(declination) + cos(latitude) * cos(declination) * cos(hourAngle)
        return asin(max(-1, min(1, sinElevation))) / rad
    }
}
