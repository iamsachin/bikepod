import CoreBluetooth
import Foundation
import Observation

/// Turn direction, encoded as the first letter of the pod message.
enum TurnDir: String, CaseIterable, Identifiable {
    case left = "L", right = "R", straight = "S"
    var id: String { rawValue }
    var label: String {
        switch self {
        case .left: "Left"
        case .right: "Right"
        case .straight: "Straight"
        }
    }
}

/// Bluetooth LE link to the handlebar pod (see firmware/pod/BLE_Link.h).
/// The pod is a GATT server; we write one text line to its "Nav" characteristic, e.g.
/// `R|400|MG Rd`, `REROUTE`, `ARRIVED|7.3|29`, `CRUISE|42|21:35`.
@Observable
final class PodLink: NSObject {
    static let serviceUUID = CBUUID(string: "a30be272-f18f-4629-b9d2-b4dc872e5d05")
    static let navCharUUID = CBUUID(string: "50632787-1b5b-4788-a4ee-8ff5dc145962")
    /// Pod -> phone lines (notifications), e.g. NOWPLAYING|7|title|artist
    static let eventCharUUID = CBUUID(string: "6e0a3a5c-6f2b-4c1d-9c9a-2b6c5d7e8f10")

    @ObservationIgnored private var eventHandlers: [(String) -> Void] = []

    /// Calls `handler` (on the main queue) with each line the pod sends (NOWPLAYING, LEAN, ...)
    func onEvent(_ handler: @escaping (String) -> Void) {
        eventHandlers.append(handler)
    }

    enum State: String {
        case bluetoothOff = "Bluetooth is off"
        case scanning = "Looking for BikePod…"
        case connecting = "Connecting…"
        case connected = "Connected"
    }

    private(set) var state: State = .bluetoothOff
    private(set) var lastSent: String = ""
    /// Goes up by one on every (re)connection, so senders know when the pod needs a full resend
    private(set) var connectionID = 0
    /// The pod's battery, 0-100, as it last reported it (nil until it does, or when not connected)
    private(set) var podBattery: Int?
    /// When the current connection was made
    private(set) var connectedAt = Date.distantPast

    private var central: CBCentralManager!
    private var pod: CBPeripheral?
    private var navChar: CBCharacteristic?
    private var recheck: Timer?
    private static let savedPodKey = "podIdentifier"

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func send(_ dir: TurnDir, metres: Int, road: String) {
        send(line: "\(dir.rawValue)|\(max(0, metres))|\(road)")
    }

    /// How urgent a line is. Live lines (turn, pose, route...) always go before bulk ones (map areas),
    /// so a map download never delays what the rider sees.
    enum Priority { case live, bulk }

    private var liveQueue: [(kind: String, line: String)] = []   // newest line per kind only
    private var bulkQueue: [String] = []

    /// Queues one message line (see BLE_Link.h on the pod for the formats).
    /// - Parameter kind: live lines of the same kind replace each other while waiting, so a
    ///   backlog never sends stale positions.
    func send(line: String, kind: String? = nil, priority: Priority = .live) {
        guard pod != nil, navChar != nil else { return }
        switch priority {
        case .live:
            if let kind, let i = liveQueue.firstIndex(where: { $0.kind == kind }) {
                liveQueue[i].line = line
            } else {
                liveQueue.append((kind ?? UUID().uuidString, line))
            }
        case .bulk:
            bulkQueue.append(line)
        }
        pump()
    }

    /// Writes "without response": Bluetooth's link layer already guarantees delivery, and not
    /// waiting for a reply per line lets many lines go in each radio exchange (a map area of
    /// ~250 lines took ~7 s with replies). iOS says when it has room for more.
    private func pump() {
        guard let pod, let navChar else { return }
        while pod.canSendWriteWithoutResponse {
            let line: String
            if !liveQueue.isEmpty {
                line = liveQueue.removeFirst().line
                // Shown in Settings: the short live lines only (not map or album art data), one line
                lastSent = line.count > 48 ? String(line.prefix(48)) + "…" : line
            } else if !bulkQueue.isEmpty {
                line = bulkQueue.removeFirst()
            } else {
                return
            }
            var data = Data(line.utf8)
            // The pod accepts up to 512 bytes (a 400 byte cut once chopped the end off long route lines)
            if data.count > 512 { data = data.prefix(512) }
            // A line longer than one packet needs the slower acknowledged "long write"
            let fits = data.count <= pod.maximumWriteValueLength(for: .withoutResponse)
            pod.writeValue(data, for: navChar, type: fits ? .withoutResponse : .withResponse)
        }
    }

    private func clearQueues() {
        liveQueue.removeAll()
        bulkQueue.removeAll()
    }

    private func startScan() {
        state = .scanning
        // Once paired, iOS keeps its own connection to the pod (for calls and music), and the pod
        // stops advertising while connected: so look for a pod iOS is already connected to first
        if let connected = central.retrieveConnectedPeripherals(withServices: [Self.serviceUUID]).first {
            use(connected)
            return
        }
        central.scanForPeripherals(withServices: [Self.serviceUUID])
        // iOS may connect to the pod while we scan (it reconnects paired accessories itself):
        // keep checking for that too
        recheck?.invalidate()
        recheck = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] timer in
            guard let self, self.state == .scanning else { timer.invalidate(); return }
            if let connected = self.central.retrieveConnectedPeripherals(withServices: [Self.serviceUUID]).first {
                timer.invalidate()
                self.use(connected)
            }
        }
    }

    private func use(_ peripheral: CBPeripheral) {
        central.stopScan()
        pod = peripheral
        peripheral.delegate = self
        state = .connecting
        central.connect(peripheral)          // already connected: this just opens it for this app
    }
}

extension PodLink: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard central.state == .poweredOn else {
            state = .bluetoothOff
            return
        }
        // The pod we used before: ask iOS to connect to it whenever it's around. Unlike a scan,
        // this request keeps working while the app is in the background (the phone locked on
        // the handlebar) and wakes the app when the pod appears.
        if let saved = UserDefaults.standard.string(forKey: Self.savedPodKey).flatMap(UUID.init(uuidString:)),
           let known = central.retrievePeripherals(withIdentifiers: [saved]).first {
            use(known)
        } else {
            startScan()
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        use(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        UserDefaults.standard.set(peripheral.identifier.uuidString, forKey: Self.savedPodKey)
        peripheral.discoverServices([Self.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        startScan()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        navChar = nil
        podBattery = nil
        clearQueues()
        // Ask to reconnect straight away (works in the background, no scan needed)
        use(peripheral)
    }
}

extension PodLink: CBPeripheralDelegate {
    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        pump()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == Self.eventCharUUID, let data = characteristic.value,
              let line = String(data: data, encoding: .utf8) else { return }
        if line.hasPrefix("BATTERY|") {
            podBattery = Int(line.dropFirst("BATTERY|".count))
            return
        }
        if !line.hasPrefix("LEAN|") { DebugLog.write("LINK event \(line.prefix(80))") }   // LEAN: every 2 s
        for handler in eventHandlers { handler(line) }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == Self.serviceUUID }) else { return }
        peripheral.discoverCharacteristics([Self.navCharUUID, Self.eventCharUUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        navChar = service.characteristics?.first(where: { $0.uuid == Self.navCharUUID })
        if let events = service.characteristics?.first(where: { $0.uuid == Self.eventCharUUID }) {
            peripheral.setNotifyValue(true, for: events)
            DebugLog.write("LINK events channel found")
        } else {
            DebugLog.write("LINK no events channel (characteristics: \(service.characteristics?.map(\.uuid.uuidString) ?? []))")
        }
        if navChar != nil {
            connectionID += 1
            connectedAt = Date()
            state = .connected
        }
    }
}
