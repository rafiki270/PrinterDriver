import CoreBluetooth
import Foundation

// Apple's two Bluetooth paths, as ``BluetoothTransport`` implementations
// (docs/compatibility-brief.md §25).
//
// -- VERIFICATION STATUS ---------------------------------------------------------------
//
// SKELETONS. These compile, and their plumbing into the custom-transport ABI is unit
// tested through a scripted transport in PrinterDriverTests — which proves the boundary,
// the retention, the threading contract and the byte accounting, and proves nothing
// whatsoever about CoreBluetooth or ExternalAccessory. Neither class below has been run
// against a printer. The service and characteristic UUIDs a given printer uses, the
// write-with/without-response choice, the MTU behaviour and the MFi protocol string are
// all model-specific and are the things a real device will correct first.
//
// They are here because the shape is the deliverable: an application that wants
// Bluetooth today subclasses or replaces these, and gets the ordered fence, the
// correlation token and the honest grading for free. What it must not do is reimplement
// completion logic on its side of the boundary.
//
// -- Which path a given printer needs --------------------------------------------------
//
// Not interchangeable, and not derivable from "the printer has Bluetooth":
//
//   * BLE (``CoreBluetoothTransport``) — an Epson TM-P20II advertises its LE radio as
//     `TM-P20II-xxxxxx-L`, distinct from the Classic `TM-P20II-xxxxxx`. Works on iOS
//     without MFi enrolment.
//   * MFi / ExternalAccessory (``ExternalAccessoryTransport``) — Bluetooth Classic on
//     iOS, which is only reachable at all for accessories in Apple's MFi programme.
//     Citizen sells MFi variants of the CMP portables and the CT-S4500 for this reason.
//   * Classic SPP — on macOS and Linux a plain byte stream; on iOS, not reachable
//     without MFi, which is why the two above are separate types rather than options.
//
// This is what ``BluetoothCapabilities`` records, and why it is five booleans and not
// one.

// MARK: - BLE

/// A BLE printer, over CoreBluetooth.
///
/// - Important: A skeleton — see the note at the top of this file. In particular
///   ``serviceUUID`` and the characteristic UUIDs are model-specific and the defaults
///   here are placeholders, not documented values for any printer.
public final class CoreBluetoothTransport: NSObject, BluetoothTransport, @unchecked Sendable {
  /// How the peripheral is identified. A printer that has been seen before is best
  /// reconnected by identifier; a fresh one has to be found by advertised service.
  public enum Target: Sendable {
    /// `CBPeripheral.identifier`, as returned by a previous scan. Stable per device per
    /// host, and **not** the Bluetooth address, which iOS never exposes.
    case identifier(UUID)
    /// The local name the printer advertises, e.g. `"TM-P20II-123456-L"`.
    case advertisedName(String)
  }

  private let target: Target
  private let serviceUUID: CBUUID
  private let writeCharacteristicUUID: CBUUID
  private let notifyCharacteristicUUID: CBUUID
  private let connectTimeout: TimeInterval

  /// CoreBluetooth's own queue. Every delegate callback arrives here, which is a
  /// different thread from the core's worker — exactly what ``BluetoothLink`` requires,
  /// and the reason no delivery below happens on the writing thread.
  private let queue = DispatchQueue(label: "com.printerdriver.corebluetooth")

  private var central: CBCentralManager?
  private var peripheral: CBPeripheral?
  private var writeCharacteristic: CBCharacteristic?
  private var link: BluetoothLink?

  private let lock = NSLock()
  private var readySemaphore: DispatchSemaphore?
  private var ready = false

  public init(
    target: Target,
    serviceUUID: CBUUID,
    writeCharacteristicUUID: CBUUID,
    notifyCharacteristicUUID: CBUUID,
    connectTimeout: TimeInterval = 15
  ) {
    self.target = target
    self.serviceUUID = serviceUUID
    self.writeCharacteristicUUID = writeCharacteristicUUID
    self.notifyCharacteristicUUID = notifyCharacteristicUUID
    self.connectTimeout = connectTimeout
    super.init()
  }

  public var endpointDescription: String {
    switch target {
    case .identifier(let uuid): return "bt-le:\(uuid.uuidString)"
    case .advertisedName(let name): return "bt-le:\(name)"
    }
  }

  public func open(link: BluetoothLink) throws {
    lock.lock()
    self.link = link
    ready = false
    let semaphore = DispatchSemaphore(value: 0)
    readySemaphore = semaphore
    lock.unlock()

    // Constructing the manager starts the state machine; everything else happens in the
    // delegate callbacks below, on `queue`.
    central = CBCentralManager(delegate: self, queue: queue)

    guard semaphore.wait(timeout: .now() + connectTimeout) == .success else {
      close()
      throw PrinterDriverError("BLE printer \(endpointDescription) did not become ready")
    }
    lock.lock()
    let connected = ready
    lock.unlock()
    guard connected else {
      close()
      throw PrinterDriverError("BLE printer \(endpointDescription) refused the connection")
    }
  }

  public func write(_ bytes: UnsafeRawBufferPointer) throws -> Int {
    lock.lock()
    let target = peripheral
    let characteristic = writeCharacteristic
    lock.unlock()
    guard let target, let characteristic else {
      throw PrinterDriverError("BLE printer \(endpointDescription) is not connected")
    }
    guard let base = bytes.baseAddress, !bytes.isEmpty else { return 0 }

    // Chunked to the negotiated ATT payload. A receipt is far larger than one write, and
    // a BLE stack silently truncating an oversized one is how half a ticket prints.
    let limit = max(1, target.maximumWriteValueLength(for: .withResponse))
    var sent = 0
    while sent < bytes.count {
      let size = min(limit, bytes.count - sent)
      let chunk = Data(bytes: base.advanced(by: sent), count: size)
      target.writeValue(chunk, for: characteristic, type: .withResponse)
      sent += size
    }
    return sent
  }

  public func close() {
    lock.lock()
    let central = self.central
    let peripheral = self.peripheral
    self.peripheral = nil
    writeCharacteristic = nil
    ready = false
    let semaphore = readySemaphore
    readySemaphore = nil
    lock.unlock()

    if let peripheral {
      central?.cancelPeripheralConnection(peripheral)
    }
    // Releases an `open` still waiting, so a close during connection does not leave the
    // core's worker parked for the whole timeout.
    semaphore?.signal()
  }

  private func finishOpen(succeeded: Bool) {
    lock.lock()
    ready = succeeded
    let semaphore = readySemaphore
    readySemaphore = nil
    lock.unlock()
    semaphore?.signal()
  }
}

extension CoreBluetoothTransport: CBCentralManagerDelegate {
  public func centralManagerDidUpdateState(_ central: CBCentralManager) {
    guard central.state == .poweredOn else {
      // Unauthorized, unsupported and powered-off are all "no link", and the distinction
      // belongs to the app's UI, not to a print job.
      if central.state != .unknown && central.state != .resetting {
        finishOpen(succeeded: false)
      }
      return
    }
    switch target {
    case .identifier(let uuid):
      if let known = central.retrievePeripherals(withIdentifiers: [uuid]).first {
        connect(central, to: known)
        return
      }
      central.scanForPeripherals(withServices: [serviceUUID])
    case .advertisedName:
      central.scanForPeripherals(withServices: [serviceUUID])
    }
  }

  public func centralManager(
    _ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
    advertisementData: [String: Any], rssi RSSI: NSNumber
  ) {
    switch target {
    case .identifier(let uuid) where peripheral.identifier == uuid:
      connect(central, to: peripheral)
    case .advertisedName(let name):
      let advertised = advertisementData[CBAdvertisementDataLocalNameKey] as? String
      if advertised == name || peripheral.name == name {
        connect(central, to: peripheral)
      }
    default:
      break
    }
  }

  public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
    peripheral.delegate = self
    peripheral.discoverServices([serviceUUID])
  }

  public func centralManager(
    _ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?
  ) {
    finishOpen(succeeded: false)
  }

  public func centralManager(
    _ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?
  ) {
    lock.lock()
    let link = self.link
    let wasReady = ready
    ready = false
    lock.unlock()
    if wasReady {
      // Tells the core straight away rather than letting a job in flight burn its whole
      // completion budget waiting for a fence that can no longer arrive.
      link?.linkDropped(error?.localizedDescription ?? "the BLE link dropped")
    } else {
      finishOpen(succeeded: false)
    }
  }

  private func connect(_ central: CBCentralManager, to peripheral: CBPeripheral) {
    central.stopScan()
    lock.lock()
    self.peripheral = peripheral
    lock.unlock()
    central.connect(peripheral)
  }
}

extension CoreBluetoothTransport: CBPeripheralDelegate {
  public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
    guard error == nil, let service = peripheral.services?.first(where: { $0.uuid == serviceUUID })
    else {
      finishOpen(succeeded: false)
      return
    }
    peripheral.discoverCharacteristics(
      [writeCharacteristicUUID, notifyCharacteristicUUID], for: service)
  }

  public func peripheral(
    _ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?
  ) {
    guard error == nil, let characteristics = service.characteristics else {
      finishOpen(succeeded: false)
      return
    }
    guard let write = characteristics.first(where: { $0.uuid == writeCharacteristicUUID }),
      let notify = characteristics.first(where: { $0.uuid == notifyCharacteristicUUID })
    else {
      // A printer with no notify characteristic is a write-only device: it can print, but
      // nothing it does can ever be confirmed. Refusing here is better than connecting and
      // reporting grade E jobs that the caller believes are fenced.
      finishOpen(succeeded: false)
      return
    }
    lock.lock()
    writeCharacteristic = write
    lock.unlock()
    peripheral.setNotifyValue(true, for: notify)
    finishOpen(succeeded: true)
  }

  public func peripheral(
    _ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?
  ) {
    guard error == nil, let value = characteristic.value, !value.isEmpty else { return }
    lock.lock()
    let link = self.link
    lock.unlock()
    // On CoreBluetooth's queue, never on the core's worker: the contract in
    // ``BluetoothLink/deliver(_:)-swift.method``.
    link?.deliver(value)
  }
}

// MARK: - MFi

#if canImport(ExternalAccessory)

  import ExternalAccessory

  /// A Bluetooth Classic printer on iOS, over the MFi ExternalAccessory framework.
  ///
  /// - Important: A skeleton — see the note at the top of this file. It also requires
  ///   app-side setup that no SDK can supply: the accessory's protocol string must be
  ///   listed under `UISupportedExternalAccessoryProtocols` in the app's Info.plist, and
  ///   the accessory must be MFi-certified for that string. Without both, no session opens.
  public final class ExternalAccessoryTransport: NSObject, BluetoothTransport, @unchecked Sendable
  {
    /// The MFi protocol string, e.g. `"com.epson.escpos"`. Vendor-specific, published by
    /// the manufacturer, and not guessable.
    private let protocolString: String
    private let serialNumber: String?

    private let lock = NSLock()
    private var session: EASession?
    private var link: BluetoothLink?
    private var reader: Thread?
    private var stopping = false

    public init(protocolString: String, serialNumber: String? = nil) {
      self.protocolString = protocolString
      self.serialNumber = serialNumber
      super.init()
    }

    public var endpointDescription: String {
      "bt-mfi:\(protocolString)" + (serialNumber.map { ":\($0)" } ?? "")
    }

    public func open(link: BluetoothLink) throws {
      let accessories = EAAccessoryManager.shared().connectedAccessories
      let match = accessories.first { accessory in
        guard accessory.protocolStrings.contains(protocolString) else { return false }
        guard let serialNumber else { return true }
        return accessory.serialNumber == serialNumber
      }
      guard let match, let session = EASession(accessory: match, forProtocol: protocolString) else {
        throw PrinterDriverError("no MFi accessory for \(endpointDescription)")
      }
      lock.lock()
      self.session = session
      self.link = link
      stopping = false
      lock.unlock()

      session.outputStream?.schedule(in: .current, forMode: .default)
      session.outputStream?.open()

      // The input stream runs on its own thread so bytes are delivered off the core's
      // worker, as ``BluetoothLink`` requires. A run loop, because Foundation streams
      // deliver through one.
      let reader = Thread { [weak self] in self?.readerLoop(session) }
      reader.name = "com.printerdriver.mfi-reader"
      lock.lock()
      self.reader = reader
      lock.unlock()
      reader.start()
    }

    public func write(_ bytes: UnsafeRawBufferPointer) throws -> Int {
      lock.lock()
      let output = session?.outputStream
      lock.unlock()
      guard let output, let base = bytes.baseAddress else {
        throw PrinterDriverError("MFi session \(endpointDescription) is not open")
      }
      let source = base.assumingMemoryBound(to: UInt8.self)
      var sent = 0
      while sent < bytes.count {
        let wrote = output.write(source.advanced(by: sent), maxLength: bytes.count - sent)
        if wrote <= 0 {
          // Returned, not thrown: how far the write got is what separates a receipt that
          // certainly did not print from one that may have printed most of itself.
          return sent
        }
        sent += wrote
      }
      return sent
    }

    public func close() {
      lock.lock()
      stopping = true
      let session = self.session
      self.session = nil
      self.reader = nil
      lock.unlock()
      session?.inputStream?.close()
      session?.outputStream?.close()
    }

    private func readerLoop(_ session: EASession) {
      guard let input = session.inputStream else { return }
      input.schedule(in: .current, forMode: .default)
      input.open()
      var buffer = [UInt8](repeating: 0, count: 512)
      while true {
        lock.lock()
        let done = stopping
        let link = self.link
        lock.unlock()
        if done { break }
        RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.05))
        guard input.hasBytesAvailable else { continue }
        let read = input.read(&buffer, maxLength: buffer.count)
        if read > 0 {
          buffer.withUnsafeBytes { raw -> Void in
            link?.deliver(UnsafeRawBufferPointer(rebasing: raw[0..<read]))
          }
        } else if read < 0 {
          link?.linkDropped("the MFi input stream failed")
          break
        }
      }
      input.close()
    }
  }

#endif
