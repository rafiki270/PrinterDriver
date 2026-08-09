import Foundation

// §4's GATT discovery algorithm, driven by a real radio
// (docs/wire-protocols.md §4, docs/compatibility-brief.md §25).
//
// -- VERIFICATION STATUS ---------------------------------------------------------------
//
// The selection, chunking and backpressure logic below is unit tested in full, with fake
// descriptors and no radio — see BluetoothGATTTests. What is *not* tested here, and cannot
// be without hardware, is CoreBluetooth itself: the discovery callbacks, the MTU the stack
// actually negotiates, and whether a given printer's module behaves the way its UUIDs
// suggest. Those are the parts a real device corrects first.
//
// -- Why this exists next to CoreBluetoothTransport ------------------------------------
//
// ``CoreBluetoothTransport`` is told which service and characteristics to use. That is the
// right shape once somebody knows, and useless before: the overwhelmingly common case is a
// printer whose manual says "Bluetooth 4.0" and nothing else, whose UUIDs are whatever
// serial-over-GATT module the ODM had in stock, and which a caller cannot name because
// nobody published them. This class is that case — it discovers, decides and records what
// it decided.
//
// Everything above the boundary is unchanged. The transport answers "can bytes move?" and
// nothing else; the ordered fence, the `GS ( H` correlation token, the preflight and the
// grading stay in the core, so a printer found by probe reports exactly the grade a
// printer found by configuration reports. Discovery is allowed to be a heuristic precisely
// because completion is not.

#if canImport(CoreBluetooth)

  import CoreBluetooth

  /// A BLE printer reached through one of §4's generic serial-over-GATT profiles,
  /// discovered rather than declared.
  ///
  /// Open connects, discovers every service and characteristic on the peripheral, and runs
  /// ``BLEEndpointSelector`` over the result: an explicitly configured pair wins outright,
  /// otherwise Microchip Transparent UART, then Nordic UART, then the FFE0 family, each
  /// validated on its **properties** and not merely its UUIDs. What it settled on is
  /// readable afterwards from ``selectedEndpoint``, and worth logging: it is the first
  /// question anyone asks about a printer that half-worked.
  ///
  /// - Important: A match means a serial module was recognised. It is not a claim that the
  ///   far side speaks ESC/POS, and it is emphatically not a way to reach an Epson, Star,
  ///   Bixolon or Citizen BLE printer — §4 records all four as unpublished, and
  ///   ``BluetoothVendorFacts`` records why nothing here will ever map them onto `FFE1`.
  public final class BLEUARTTransport: NSObject, BluetoothTransport, @unchecked Sendable {
    /// How the peripheral is identified.
    public enum Target: Sendable {
      /// `CBPeripheral.identifier` from a previous scan. Stable per device per host, and
      /// **not** the Bluetooth address, which iOS never exposes.
      case identifier(UUID)
      /// The advertised local name, e.g. `"TM-P20II-123456-L"`.
      case advertisedName(String)
    }

    private let target: Target
    private let configuration: BLEEndpointConfiguration
    private let connectTimeout: TimeInterval
    private let writeTimeout: TimeInterval
    private let statusFrameTimeout: TimeInterval

    /// CoreBluetooth's own queue. Every delegate callback arrives here, which is a
    /// different thread from the core's worker — exactly what ``BluetoothLink`` requires,
    /// and the reason no delivery below happens on the writing thread.
    private let queue = DispatchQueue(label: "com.printerdriver.ble-uart")

    private let lock = NSLock()
    private var central: CBCentralManager?
    private var peripheral: CBPeripheral?
    private var outbound: CBCharacteristic?
    private var inbound: CBCharacteristic?
    private var link: BluetoothLink?
    private var endpoint: BLEEndpointSelection?
    private var pendingServices = 0
    private var discovered: [CBService] = []
    private var openSemaphore: DispatchSemaphore?
    private var isReady = false
    private var writeFailure: String?

    /// Opened by `peripheralIsReady(toSendWriteWithoutResponse:)` — §4 step (5).
    private let sendGate = BLEWriteGate()
    /// Opened by `didWriteValueFor` — the Write Request acknowledgement.
    private let ackGate = BLEWriteGate()
    /// Opened by `didUpdateValueFor` — only ever waited on when
    /// ``BLEStatusFraming/pacesOnInboundFrames`` says the framing is trusted.
    private let inboundGate = BLEWriteGate()

    /// - Parameters:
    ///   - target: which peripheral.
    ///   - configuration: what the caller already knows. The default is empty, which means
    ///     "probe §4's three families" and leaves status framing off.
    ///   - connectTimeout: how long ``open(link:)`` blocks waiting for discovery to settle.
    ///     `open` runs on the core's worker thread, which is allowed to block — it would be
    ///     waiting anyway — but not forever, or a printer that is switched off parks the
    ///     queue instead of failing it.
    ///   - writeTimeout: per-chunk budget for an acknowledgement or for room in the
    ///     without-response queue. Exceeding it produces a **short write**, reported
    ///     honestly, which is what leaves the job `unknown` rather than `done`.
    ///   - statusFrameTimeout: the between-block pause, used only when
    ///     `configuration.statusFraming` is not `.disabled`.
    public init(
      target: Target,
      configuration: BLEEndpointConfiguration = BLEEndpointConfiguration(),
      connectTimeout: TimeInterval = 15,
      writeTimeout: TimeInterval = 5,
      statusFrameTimeout: TimeInterval = 0.25
    ) {
      self.target = target
      self.configuration = configuration
      self.connectTimeout = connectTimeout
      self.writeTimeout = writeTimeout
      self.statusFrameTimeout = statusFrameTimeout
      super.init()
    }

    /// The profile discovery settled on, or `nil` before a successful ``open(link:)``.
    public var selectedEndpoint: BLEEndpointSelection? {
      lock.lock()
      defer { lock.unlock() }
      return endpoint
    }

    /// The endpoint as one loggable line, or a note that nothing has been chosen yet.
    public var selectionLogDescription: String {
      selectedEndpoint?.logDescription ?? "no BLE-UART endpoint selected"
    }

    /// Deliberately free of the discovered profile. The id derives from this string, and a
    /// printer must not change identity because a firmware update moved it from `FFE1` to
    /// `FFE2`: it is the same printer, with the same journal and the same idempotency keys.
    public var endpointDescription: String {
      switch target {
      case .identifier(let uuid): return "bt-le:\(uuid.uuidString)"
      case .advertisedName(let name): return "bt-le:\(name)"
      }
    }

    // MARK: Transport

    public func open(link: BluetoothLink) throws {
      lock.lock()
      self.link = link
      isReady = false
      endpoint = nil
      discovered = []
      pendingServices = 0
      writeFailure = nil
      let semaphore = DispatchSemaphore(value: 0)
      openSemaphore = semaphore
      lock.unlock()

      // The core builds a fresh internal transport after a link drop and calls back into
      // the same object, so gates invalidated by the previous close have to be usable
      // again or the reconnection would write nothing and report it as a short write.
      sendGate.reset()
      ackGate.reset()
      inboundGate.reset()

      // Constructing the manager starts the state machine; everything else happens in the
      // delegate callbacks, on `queue`.
      let manager = CBCentralManager(delegate: self, queue: queue)
      lock.lock()
      central = manager
      lock.unlock()

      // Blocking here is the contract, not an oversight: `open` runs on the core's worker
      // thread and GATT discovery is four round trips deep, so there is nothing useful for
      // that thread to do until the peripheral has answered or the budget is gone.
      guard semaphore.wait(timeout: .now() + connectTimeout) == .success else {
        close()
        throw PrinterDriverError(
          "BLE printer \(endpointDescription) did not finish GATT discovery within "
            + "\(Int(connectTimeout))s")
      }
      lock.lock()
      let connected = isReady && endpoint != nil
      lock.unlock()
      guard connected else {
        close()
        throw PrinterDriverError(
          "BLE printer \(endpointDescription) exposed no usable BLE-UART profile "
            + "(docs/wire-protocols.md §4)")
      }
    }

    public func write(_ bytes: UnsafeRawBufferPointer) throws -> Int {
      lock.lock()
      let peripheral = self.peripheral
      let characteristic = outbound
      let mode = endpoint?.writeMode
      lock.unlock()
      guard let peripheral, let characteristic, let mode else {
        throw PrinterDriverError("BLE printer \(endpointDescription) is not connected")
      }
      guard let base = bytes.baseAddress, !bytes.isEmpty else { return 0 }

      let writeType: CBCharacteristicWriteType =
        mode == .withResponse ? .withResponse : .withoutResponse
      // §4 step (4). Asked per write type, because the same characteristic answers
      // differently for a Request and a Command, and never assumed to be 20.
      let limit = queue.sync { peripheral.maximumWriteValueLength(for: writeType) }

      if configuration.statusFraming.pacesOnInboundFrames { inboundGate.reset() }

      var sent = 0
      for range in BLEChunking.ranges(byteCount: bytes.count, maximumWriteLength: limit) {
        if writeType == .withoutResponse {
          // §4 step (5). A Write Command issued while the queue is full is dropped
          // silently, so the loop parks on the gate until the stack says there is room —
          // re-checking the flag each time round rather than trusting one wakeup, and
          // never spinning.
          while !queue.sync(execute: { peripheral.canSendWriteWithoutResponse }) {
            guard sendGate.waitUntilOpen(timeout: writeTimeout) else { return sent }
          }
        }

        let chunk = Data(bytes: base.advanced(by: range.lowerBound), count: range.count)
        queue.sync { peripheral.writeValue(chunk, for: characteristic, type: writeType) }

        if writeType == .withResponse {
          // One chunk in flight at a time. It costs a connection interval per chunk and
          // buys the thing a receipt needs: a chunk that failed is a chunk the stack tells
          // us about, and the count returned stops at it.
          guard ackGate.waitUntilOpen(timeout: writeTimeout) else { return sent }
          lock.lock()
          let failure = writeFailure
          lock.unlock()
          if failure != nil { return sent }
        }
        sent += range.count
      }

      if configuration.statusFraming.pacesOnInboundFrames {
        // A pause between blocks, never a handshake: the result is discarded on purpose,
        // because a module that coalesced the frame into the next notification has not
        // failed and must not be treated as though it had. See ``BLEStatusFraming``.
        inboundGate.waitUntilOpen(timeout: statusFrameTimeout)
      }
      return sent
    }

    public func close() {
      lock.lock()
      let central = self.central
      let peripheral = self.peripheral
      self.peripheral = nil
      outbound = nil
      inbound = nil
      endpoint = nil
      discovered = []
      pendingServices = 0
      isReady = false
      let semaphore = openSemaphore
      openSemaphore = nil
      lock.unlock()

      // Releases any writer parked on backpressure in milliseconds rather than at the end
      // of its budget: the peripheral is going away, so waiting for room in its queue can
      // only ever time out.
      sendGate.invalidate()
      ackGate.invalidate()
      inboundGate.invalidate()

      central?.stopScan()
      if let peripheral {
        central?.cancelPeripheralConnection(peripheral)
      }
      // Releases an `open` still waiting, so a close during discovery does not leave the
      // core's worker parked for the whole connect timeout.
      semaphore?.signal()
    }

    // MARK: Internals

    private func finishOpen(succeeded: Bool) {
      lock.lock()
      isReady = succeeded
      let semaphore = openSemaphore
      openSemaphore = nil
      lock.unlock()
      semaphore?.signal()
    }

    /// Runs the selection over everything discovered, then binds the CoreBluetooth objects
    /// behind the winning description.
    private func settle(_ peripheral: CBPeripheral, services: [CBService]) {
      let descriptions = services.map(GATTServiceDescription.init)
      guard
        let selection = BLEEndpointSelector.select(
          services: descriptions, configuration: configuration)
      else {
        // Nothing on this peripheral can carry a receipt. Refusing here beats connecting
        // and failing every job later with a transport error that names no cause.
        finishOpen(succeeded: false)
        return
      }
      guard
        let service = services.first(where: { GATTUUID($0.uuid.uuidString) == selection.service }),
        let outbound = service.characteristics?.first(where: {
          GATTUUID($0.uuid.uuidString) == selection.outbound
        }),
        let inbound = service.characteristics?.first(where: {
          GATTUUID($0.uuid.uuidString) == selection.inbound
        })
      else {
        finishOpen(succeeded: false)
        return
      }

      lock.lock()
      self.outbound = outbound
      self.inbound = inbound
      endpoint = selection
      lock.unlock()

      // Subscribing is what makes the printer volunteer anything at all. It happens
      // whatever the framing setting says, because the bytes a printer sends are evidence
      // and the core's parser is the thing entitled to read them; what the setting gates is
      // this transport inventing a block handshake on top of them.
      peripheral.setNotifyValue(true, for: inbound)
      finishOpen(succeeded: true)
    }

    private func connect(_ central: CBCentralManager, to peripheral: CBPeripheral) {
      central.stopScan()
      lock.lock()
      self.peripheral = peripheral
      lock.unlock()
      central.connect(peripheral)
    }

    /// What to filter a scan by. Only a configured service can be filtered on: the whole
    /// point of the probe is that the family is unknown until the services have been read,
    /// and a printer that does not put its UART service in its advertisement packet — most
    /// do not — is invisible to a filtered scan.
    private var scanFilter: [CBUUID]? {
      configuration.service.map { [CBUUID(string: $0.canonical)] }
    }
  }

  // MARK: - Central delegate

  extension BLEUARTTransport: CBCentralManagerDelegate {
    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
      guard central.state == .poweredOn else {
        // Unauthorized, unsupported and powered-off are all "no link", and the distinction
        // belongs to the app's UI, not to a print job.
        if central.state != .unknown && central.state != .resetting {
          finishOpen(succeeded: false)
        }
        return
      }
      if case .identifier(let uuid) = target,
        let known = central.retrievePeripherals(withIdentifiers: [uuid]).first
      {
        connect(central, to: known)
        return
      }
      central.scanForPeripherals(withServices: scanFilter)
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
      // `nil` unless the caller named a service: discovering only the families we expect
      // would make the probe unable to report what it actually found, and the FFE0 service
      // is frequently published alongside several others.
      peripheral.discoverServices(scanFilter)
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
      let wasReady = isReady
      isReady = false
      lock.unlock()

      // Anything blocked on backpressure is waiting for a peripheral that is gone.
      sendGate.invalidate()
      ackGate.invalidate()
      inboundGate.invalidate()

      if wasReady {
        // Tells the core straight away rather than letting a job in flight burn its whole
        // completion budget waiting for a fence that can no longer arrive.
        link?.linkDropped(error?.localizedDescription ?? "the BLE link dropped")
      } else {
        finishOpen(succeeded: false)
      }
    }
  }

  // MARK: - Peripheral delegate

  extension BLEUARTTransport: CBPeripheralDelegate {
    public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
      guard error == nil, let services = peripheral.services, !services.isEmpty else {
        finishOpen(succeeded: false)
        return
      }
      lock.lock()
      pendingServices = services.count
      discovered = []
      lock.unlock()
      for service in services {
        // `nil`: properties are the test, so every characteristic has to be read before
        // any of them can be rejected.
        peripheral.discoverCharacteristics(nil, for: service)
      }
    }

    public func peripheral(
      _ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?
    ) {
      lock.lock()
      // A service that failed characteristic discovery is dropped rather than aborting the
      // lot: peripherals routinely carry one unreadable vendor service alongside a
      // perfectly good UART.
      if error == nil { discovered.append(service) }
      pendingServices -= 1
      let remaining = pendingServices
      let snapshot = discovered
      lock.unlock()
      guard remaining == 0 else { return }
      settle(peripheral, services: snapshot)
    }

    public func peripheral(
      _ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?
    ) {
      if let error {
        lock.lock()
        writeFailure = error.localizedDescription
        lock.unlock()
      }
      ackGate.open()
    }

    public func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
      sendGate.open()
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
      inboundGate.open()
    }
  }

  // MARK: - Bridging

  extension GATTCharacteristicProperties {
    /// The subset of CoreBluetooth's property mask §4 step (3) reasons about. Broadcast,
    /// authenticated writes and extended properties are deliberately dropped: none of them
    /// changes whether a characteristic can carry a receipt out or a status frame back.
    init(_ properties: CBCharacteristicProperties) {
      var value: GATTCharacteristicProperties = []
      if properties.contains(.read) { value.insert(.read) }
      if properties.contains(.write) { value.insert(.write) }
      if properties.contains(.writeWithoutResponse) { value.insert(.writeWithoutResponse) }
      if properties.contains(.notify) { value.insert(.notify) }
      if properties.contains(.indicate) { value.insert(.indicate) }
      self = value
    }
  }

  extension GATTServiceDescription {
    /// A discovered service, reduced to the values the selection core reasons about. The
    /// only place CoreBluetooth types cross into that core, and they cross as data.
    init(_ service: CBService) {
      self.init(
        uuid: GATTUUID(service.uuid.uuidString),
        characteristics: (service.characteristics ?? []).map {
          GATTCharacteristicDescription(
            uuid: GATTUUID($0.uuid.uuidString),
            properties: GATTCharacteristicProperties($0.properties))
        })
    }
  }

#endif
