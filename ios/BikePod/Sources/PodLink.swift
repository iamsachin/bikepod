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

    private var central: CBCentralManager!
    private var pod: CBPeripheral?
    private var navChar: CBCharacteristic?

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
            } else if !bulkQueue.isEmpty {
                line = bulkQueue.removeFirst()
            } else {
                return
            }
            var data = Data(line.utf8)
            if data.count > 400 { data = data.prefix(400) }
            // A line longer than one packet needs the slower acknowledged "long write"
            let fits = data.count <= pod.maximumWriteValueLength(for: .withoutResponse)
            pod.writeValue(data, for: navChar, type: fits ? .withoutResponse : .withResponse)
            lastSent = line
        }
    }

    private func clearQueues() {
        liveQueue.removeAll()
        bulkQueue.removeAll()
    }

    private func startScan() {
        state = .scanning
        central.scanForPeripherals(withServices: [Self.serviceUUID])
    }
}

extension PodLink: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            startScan()
        } else {
            state = .bluetoothOff
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        central.stopScan()
        pod = peripheral
        peripheral.delegate = self
        state = .connecting
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([Self.serviceUUID])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        startScan()
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        navChar = nil
        pod = nil
        clearQueues()
        startScan()   // the pod restarts advertising, so just look for it again
    }
}

extension PodLink: CBPeripheralDelegate {
    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        pump()
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == Self.serviceUUID }) else { return }
        peripheral.discoverCharacteristics([Self.navCharUUID], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        navChar = service.characteristics?.first(where: { $0.uuid == Self.navCharUUID })
        if navChar != nil {
            connectionID += 1
            state = .connected
        }
    }
}
