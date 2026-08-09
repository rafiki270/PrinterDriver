import Foundation

// The generic BLE-UART selection core (docs/wire-protocols.md §4,
// docs/compatibility-brief.md §25).
//
// -- Why this layer has no CoreBluetooth in it -----------------------------------------
//
// There is no BLE printer profile. The Bluetooth SIG never standardised one, and §4's
// verdict for generic BLE is "probe profiles, no universal printer profile" — so every
// BLE printer that is reachable without a vendor SDK is reachable because it embeds
// somebody's serial-over-GATT module, and the SDK's job is to recognise which one. That
// recognition is pure logic over a list of UUIDs and property bitmasks: it needs a radio
// to *obtain* the list and no radio at all to *reason* about it.
//
// Splitting it out is not tidiness. A heuristic that can only be exercised by pairing a
// physical printer is a heuristic nobody re-runs, and the failure it hides is the worst
// kind this SDK can ship: a characteristic whose UUID matched, whose properties did not,
// and which therefore swallowed a receipt in silence. Every rule in §4's algorithm is
// expressed here against plain values, so CI re-runs the whole of it on every commit with
// fake descriptors and no hardware — see BluetoothGATTTests.
//
// The CoreBluetooth half lives in BluetoothLEUART.swift and does exactly two things this
// file cannot: it turns `CBService`/`CBCharacteristic` into the descriptions below, and it
// moves bytes.

// MARK: - UUID

/// A GATT UUID compared the way the Bluetooth spec means it to be compared.
///
/// Two things make string equality the wrong test. First, case: `CBUUID.uuidString` is
/// uppercase, most vendor documentation is lowercase, and §4 prints the Microchip and
/// Nordic UUIDs in uppercase while the FFE family is conventionally written lowercase.
/// Second, and worse, the **short form**: a 16-bit SIG-assigned UUID such as `FFE0` is
/// shorthand for `0000FFE0-0000-1000-8000-00805F9B34FB`, and CoreBluetooth reports it in
/// whichever form it feels like — the short one for assigned numbers, the long one for
/// vendor UUIDs. A comparison that misses that equivalence rejects the single most common
/// BLE-UART module on the market while looking, in a log, exactly like a printer that was
/// not there.
///
/// So everything is folded to the canonical 128-bit uppercase form on the way in, and
/// compared there.
public struct GATTUUID: Hashable, Sendable, CustomStringConvertible, ExpressibleByStringLiteral {
  /// The canonical 128-bit form: uppercase, hyphenated, always 36 characters for anything
  /// that parsed. Text that is not a UUID at all is kept verbatim (uppercased) rather than
  /// coerced, so a typo compares unequal to everything instead of aliasing onto something.
  public let canonical: String

  /// The tail of the Bluetooth SIG base UUID, which is what a 16- or 32-bit short form
  /// expands against.
  public static let sigBaseSuffix = "-0000-1000-8000-00805F9B34FB"

  public init(_ text: String) { canonical = GATTUUID.canonicalised(text) }

  public init(stringLiteral value: StringLiteralType) { self.init(value) }

  public var description: String { canonical }

  /// The 16-bit short form when this is a SIG-assigned UUID, e.g. `"FFE1"`; `nil` for a
  /// vendor UUID. Logging shorthand only — never a comparison key.
  public var shortForm: String? {
    guard canonical.count == 36, canonical.hasSuffix(GATTUUID.sigBaseSuffix) else { return nil }
    let head = canonical.prefix(8)
    guard head.hasPrefix("0000") else { return nil }
    return String(head.dropFirst(4))
  }

  private static func canonicalised(_ text: String) -> String {
    let upper = text.uppercased()
    var hex = ""
    hex.reserveCapacity(32)
    for character in upper where character != "-" && character != "{" && character != "}"
      && character != " "
    {
      hex.append(character)
    }
    guard hex.allSatisfy({ $0.isHexDigit }) else { return upper }
    switch hex.count {
    case 4: return "0000" + hex + sigBaseSuffix
    case 8: return hex + sigBaseSuffix
    case 32: return hyphenated(hex)
    default: return upper
    }
  }

  private static func hyphenated(_ hex: String) -> String {
    var out = ""
    out.reserveCapacity(36)
    for (index, character) in hex.enumerated() {
      if index == 8 || index == 12 || index == 16 || index == 20 { out.append("-") }
      out.append(character)
    }
    return out
  }
}

// MARK: - Properties

/// A characteristic's GATT properties, as the bitmask §4 step (3) insists on checking.
///
/// "Validate **properties**, not UUIDs alone" is the whole of the rule, and it exists
/// because BLE-UART UUIDs are copied between products far more reliably than the
/// behaviour behind them: a module that exposes `FFE1` read-only, or a Nordic clone whose
/// `6E400003` is a plain readable value rather than a notify source, is a device that
/// matches by name and cannot carry a receipt.
public struct GATTCharacteristicProperties: OptionSet, Hashable, Sendable {
  public let rawValue: UInt16

  public init(rawValue: UInt16) { self.rawValue = rawValue }

  public static let read = GATTCharacteristicProperties(rawValue: 1 << 0)
  public static let write = GATTCharacteristicProperties(rawValue: 1 << 1)
  public static let writeWithoutResponse = GATTCharacteristicProperties(rawValue: 1 << 2)
  public static let notify = GATTCharacteristicProperties(rawValue: 1 << 3)
  public static let indicate = GATTCharacteristicProperties(rawValue: 1 << 4)

  /// What §4 requires of the host→printer characteristic: Write **or**
  /// WriteWithoutResponse. Nothing else can carry a byte outbound.
  public static let outbound: GATTCharacteristicProperties = [.write, .writeWithoutResponse]

  /// What §4 requires of the printer→host characteristic: Notify **or** Indicate.
  ///
  /// Read is deliberately not in this set. A readable characteristic can be polled, but a
  /// poll is the host asking rather than the printer answering, and the completion story
  /// this SDK tells is built on the printer volunteering a fence at the moment it clears
  /// the queue. Polling would produce the same grade letter from weaker evidence.
  public static let inbound: GATTCharacteristicProperties = [.notify, .indicate]

  /// Whether this characteristic can carry bytes to the printer.
  public var carriesOutbound: Bool { !isDisjoint(with: .outbound) }

  /// Whether this characteristic can carry bytes back from the printer.
  public var carriesInbound: Bool { !isDisjoint(with: .inbound) }
}

// MARK: - Descriptions

/// One discovered characteristic, reduced to the two facts selection needs.
public struct GATTCharacteristicDescription: Hashable, Sendable {
  public let uuid: GATTUUID
  public let properties: GATTCharacteristicProperties

  public init(uuid: GATTUUID, properties: GATTCharacteristicProperties) {
    self.uuid = uuid
    self.properties = properties
  }
}

/// One discovered service and its characteristics.
///
/// A peripheral may publish the same service UUID more than once — duplicated by a buggy
/// firmware, or a genuine second instance — so selection walks every instance rather than
/// stopping at the first, and the first *complete* pair wins.
public struct GATTServiceDescription: Hashable, Sendable {
  public let uuid: GATTUUID
  public let characteristics: [GATTCharacteristicDescription]

  public init(uuid: GATTUUID, characteristics: [GATTCharacteristicDescription]) {
    self.uuid = uuid
    self.characteristics = characteristics
  }
}

// MARK: - Probe profiles

/// One of the serial-over-GATT families §4 lists as a detection candidate.
///
/// - Important: "Detection candidates, not a standard." A match means a module was
///   recognised, not that the device on the far side speaks ESC/POS — that claim comes
///   from the capability profile, never from the transport.
public struct BLEUARTProfile: Hashable, Sendable {
  /// Which family a selection came from, carried into the result so a log can say it.
  public enum Identity: String, Hashable, Sendable, CaseIterable {
    /// Step (1): the caller named the service and characteristics itself.
    case configured
    /// Step (2), first: Microchip Transparent UART.
    case microchipTransparentUART
    /// Step (2), second: Nordic UART Service.
    case nordicUART
    /// Step (2), third: the FFE0 family — the anonymous serial modules.
    case ffeFamily

    /// The name a support engineer will recognise six months later.
    public var displayName: String {
      switch self {
      case .configured: return "configured pair"
      case .microchipTransparentUART: return "Microchip Transparent UART"
      case .nordicUART: return "Nordic UART Service"
      case .ffeFamily: return "FFE0 family"
      }
    }
  }

  public let identity: Identity
  public let service: GATTUUID
  /// Host→printer candidates, most likely first.
  public let outboundCandidates: [GATTUUID]
  /// Printer→host candidates, most likely first.
  public let inboundCandidates: [GATTUUID]

  public init(
    identity: Identity, service: GATTUUID, outboundCandidates: [GATTUUID],
    inboundCandidates: [GATTUUID]
  ) {
    self.identity = identity
    self.service = service
    self.outboundCandidates = outboundCandidates
    self.inboundCandidates = inboundCandidates
  }

  /// Microchip Transparent UART (§4). Distinct TX and RX characteristics, which is why it
  /// is probed first: a two-characteristic match is unambiguous in a way the FFE family's
  /// one-characteristic variants are not.
  public static let microchipTransparentUART = BLEUARTProfile(
    identity: .microchipTransparentUART,
    service: "49535343-FE7D-4AE5-8FA9-9FAFD205E455",
    outboundCandidates: ["49535343-8841-43F4-A8D4-ECBE34729BB3"],
    inboundCandidates: ["49535343-1E4D-4BD9-BA61-23C647249616"])

  /// Nordic UART Service (§4): `6E400002` Write/WriteWithoutResponse out, `6E400003`
  /// Notify back.
  public static let nordicUART = BLEUARTProfile(
    identity: .nordicUART,
    service: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E",
    outboundCandidates: ["6E400002-B5A3-F393-E0A9-E50E24DCCA9E"],
    inboundCandidates: ["6E400003-B5A3-F393-E0A9-E50E24DCCA9E"])

  /// The FFE0 family (§4): "TX often FFE1 (sometimes FFE2); RX often FFE1 Notify or
  /// FFE2."
  ///
  /// Both characteristics appear in both candidate lists on purpose. The commonest wiring
  /// of these modules is a *single* `FFE1` carrying Write, WriteWithoutResponse and Notify
  /// at once, so outbound and inbound legitimately resolve to the same characteristic; the
  /// next commonest splits them across `FFE1` and `FFE2` in either direction. Properties
  /// decide which, which is exactly why properties and not UUIDs are the test.
  public static let ffeFamily = BLEUARTProfile(
    identity: .ffeFamily,
    service: "0000FFE0-0000-1000-8000-00805F9B34FB",
    outboundCandidates: [
      "0000FFE1-0000-1000-8000-00805F9B34FB", "0000FFE2-0000-1000-8000-00805F9B34FB",
    ],
    inboundCandidates: [
      "0000FFE1-0000-1000-8000-00805F9B34FB", "0000FFE2-0000-1000-8000-00805F9B34FB",
    ])

  /// §4 step (2), verbatim: Microchip TUS → Nordic NUS → FFE0 family.
  ///
  /// The order is not alphabetical and not arbitrary. It runs from most specific to least:
  /// the Microchip and Nordic UUIDs are vendor-allocated and effectively unique to their
  /// module, while `FFE0` is a SIG-range number that any firmware can and does reuse for
  /// unrelated purposes. Probing the specific ones first means a device carrying both is
  /// driven through the one whose behaviour is known.
  ///
  /// No vendor profile appears here, and none ever should: §4 records Epson, Star and
  /// Bixolon BLE as SDK-only with the GATT map unpublished, so auto-selecting a family for
  /// them would be guessing — see ``BluetoothVendorFacts``.
  public static let probeOrder: [BLEUARTProfile] = [
    .microchipTransparentUART, .nordicUART, .ffeFamily,
  ]
}

// MARK: - Selection result

/// Which ATT write the outbound characteristic is driven with.
public enum BLEWriteMode: String, Hashable, Sendable, CaseIterable {
  /// Write Request. Every chunk is acknowledged by the peripheral, so the stack itself
  /// provides the flow control and a failed chunk is reportable.
  case withResponse
  /// Write Command. Faster, unacknowledged, and the reason
  /// ``BLEWriteGate`` exists: the only backpressure signal is
  /// `canSendWriteWithoutResponse`.
  case withoutResponse
}

/// How the printer→host characteristic pushes.
public enum BLEInboundDelivery: String, Hashable, Sendable, CaseIterable {
  case notify
  case indicate
}

/// What discovery settled on — recorded rather than merely acted upon, because "which
/// module did we decide this printer was?" is the first question asked when a site reports
/// half a receipt, and it is unanswerable after the fact unless it was logged at the time.
public struct BLEEndpointSelection: Hashable, Sendable {
  /// Which of §4's families matched, or ``BLEUARTProfile/Identity/configured``.
  public let profile: BLEUARTProfile.Identity
  public let service: GATTUUID
  public let outbound: GATTUUID
  public let inbound: GATTUUID
  /// Whether writes go out with or without response.
  public let writeMode: BLEWriteMode
  public let inboundDelivery: BLEInboundDelivery

  public init(
    profile: BLEUARTProfile.Identity, service: GATTUUID, outbound: GATTUUID, inbound: GATTUUID,
    writeMode: BLEWriteMode, inboundDelivery: BLEInboundDelivery
  ) {
    self.profile = profile
    self.service = service
    self.outbound = outbound
    self.inbound = inbound
    self.writeMode = writeMode
    self.inboundDelivery = inboundDelivery
  }

  /// One line for the log.
  public var logDescription: String {
    let out = outbound.shortForm ?? outbound.canonical
    let back = inbound.shortForm ?? inbound.canonical
    return
      "\(profile.displayName) · service \(service.shortForm ?? service.canonical)"
      + " · out \(out) (\(writeMode.rawValue)) · in \(back) (\(inboundDelivery.rawValue))"
  }
}

// MARK: - Status framing

/// Whether the inbound channel is trusted enough to pace writes against — §4 step (6),
/// "keep status/checked-block disabled unless notification framing is documented or
/// successfully probed".
///
/// The default is ``disabled`` and that is not conservatism for its own sake. A generic
/// BLE-UART bridge documents nothing about *framing*: it forwards a serial stream through
/// ATT notifications, and where a notification boundary falls is a property of the
/// module's buffering, not of the printer's reply. The same status byte can arrive alone,
/// glued to the tail of an earlier response, or split across two notifications on the next
/// connection event. Building a block-level handshake on that — send a block, wait for the
/// frame, send the next — produces a writer that stalls forever on a frame it will never
/// recognise, on a device that was working perfectly.
///
/// Raw bytes are always delivered to the core regardless of this setting: what the printer
/// actually said is evidence, and the core's own parser is the thing entitled to interpret
/// it. What this flag gates is the *transport* inventing a handshake on top.
public enum BLEStatusFraming: String, Hashable, Sendable, CaseIterable {
  /// Default. Writes are paced by ATT flow control alone.
  case disabled
  /// The module's notification framing is documented by its manufacturer.
  case documented
  /// The framing was probed successfully against this device and the result recorded.
  case probed

  /// Whether the transport may pause between blocks waiting for an inbound frame.
  public var pacesOnInboundFrames: Bool { self != .disabled }
}

// MARK: - Configuration

/// What the caller knows before discovery starts.
///
/// Empty is the normal case and means "probe §4's three families".
public struct BLEEndpointConfiguration: Hashable, Sendable {
  /// The service to use, if the caller knows it. When set, **only** this service is
  /// considered.
  public var service: GATTUUID?
  /// The host→printer characteristic, if the caller knows it.
  public var outboundCharacteristic: GATTUUID?
  /// The printer→host characteristic, if the caller knows it.
  public var inboundCharacteristic: GATTUUID?

  /// Prefer Write Command over Write Request when the outbound characteristic offers
  /// both.
  ///
  /// Off by default. With response, a chunk is acknowledged before the next goes out,
  /// which costs one connection interval per chunk and buys the guarantee that a chunk
  /// which vanished is *reported* as having vanished. Without response is several times
  /// faster and silent about loss, which for a receipt is the wrong trade unless the
  /// caller has measured that this device needs it.
  public var prefersWriteWithoutResponse: Bool

  /// See ``BLEStatusFraming``. Off by default, per §4 step (6).
  public var statusFraming: BLEStatusFraming

  public init(
    service: GATTUUID? = nil,
    outboundCharacteristic: GATTUUID? = nil,
    inboundCharacteristic: GATTUUID? = nil,
    prefersWriteWithoutResponse: Bool = false,
    statusFraming: BLEStatusFraming = .disabled
  ) {
    self.service = service
    self.outboundCharacteristic = outboundCharacteristic
    self.inboundCharacteristic = inboundCharacteristic
    self.prefersWriteWithoutResponse = prefersWriteWithoutResponse
    self.statusFraming = statusFraming
  }

  /// Whether the caller named anything at all — step (1) of the algorithm.
  public var namesAnEndpoint: Bool {
    service != nil || outboundCharacteristic != nil || inboundCharacteristic != nil
  }
}

// MARK: - Selection

/// §4's GATT discovery algorithm, as a function of descriptions.
public enum BLEEndpointSelector {
  /// Picks the endpoint pair to drive a printer with, or `nil` when nothing on the
  /// peripheral can carry a receipt.
  ///
  /// The algorithm, in the order §4 states it:
  ///
  /// 1. **An explicitly configured pair wins outright.** If it is present and its
  ///    properties check out, it is used; if it is absent, or present with the wrong
  ///    properties, the answer is `nil` — the probe is **not** run as a fallback. A caller
  ///    that named a service made a claim about the device, and quietly driving a
  ///    different characteristic because the named one did not work is the class of
  ///    silent substitution this SDK exists to refuse. A configuration that names only the
  ///    service still narrows to that service and picks characteristics by property.
  /// 2. Otherwise probe ``BLEUARTProfile/probeOrder`` — Microchip, Nordic, FFE0.
  /// 3. Validate **properties**, not UUIDs. A candidate whose UUID matches and whose
  ///    properties do not is rejected, and the probe carries on to the next family rather
  ///    than stopping on the near-miss.
  ///
  /// - Returns: the winning pair, with the family it came from and the write mode it will
  ///   be driven with, so the caller can log both.
  public static func select(
    services: [GATTServiceDescription],
    configuration: BLEEndpointConfiguration = BLEEndpointConfiguration()
  ) -> BLEEndpointSelection? {
    if configuration.namesAnEndpoint {
      return selectConfigured(services: services, configuration: configuration)
    }
    for profile in BLEUARTProfile.probeOrder {
      if let selection = probe(profile, services: services, configuration: configuration) {
        return selection
      }
    }
    return nil
  }

  // Step (1).
  private static func selectConfigured(
    services: [GATTServiceDescription], configuration: BLEEndpointConfiguration
  ) -> BLEEndpointSelection? {
    let candidates = configuration.service.map { named in services.filter { $0.uuid == named } }
    for service in candidates ?? services {
      guard
        let outbound = characteristic(
          in: service, named: configuration.outboundCharacteristic, requiring: .outbound),
        let inbound = characteristic(
          in: service, named: configuration.inboundCharacteristic, requiring: .inbound)
      else { continue }
      return selection(
        identity: .configured, service: service.uuid, outbound: outbound, inbound: inbound,
        configuration: configuration)
    }
    return nil
  }

  // Steps (2) and (3).
  private static func probe(
    _ profile: BLEUARTProfile, services: [GATTServiceDescription],
    configuration: BLEEndpointConfiguration
  ) -> BLEEndpointSelection? {
    for service in services where service.uuid == profile.service {
      guard
        let outbound = characteristic(
          in: service, anyOf: profile.outboundCandidates, requiring: .outbound),
        let inbound = characteristic(
          in: service, anyOf: profile.inboundCandidates, requiring: .inbound)
      else {
        // The near-miss. A service whose UUID is this family's but whose characteristics
        // cannot carry traffic is not this family, and pretending otherwise would stop the
        // probe on the one candidate guaranteed not to work.
        continue
      }
      return selection(
        identity: profile.identity, service: service.uuid, outbound: outbound, inbound: inbound,
        configuration: configuration)
    }
    return nil
  }

  /// The first candidate UUID, **in candidate order**, that is present and carries the
  /// required properties. Candidate order is preference order, so the outer loop is over
  /// the UUIDs and not over the discovered characteristics.
  private static func characteristic(
    in service: GATTServiceDescription, anyOf candidates: [GATTUUID],
    requiring required: GATTCharacteristicProperties
  ) -> GATTCharacteristicDescription? {
    for candidate in candidates {
      if let match = service.characteristics.first(where: {
        $0.uuid == candidate && !$0.properties.isDisjoint(with: required)
      }) {
        return match
      }
    }
    return nil
  }

  /// The configured variant: an explicit UUID when the caller gave one, otherwise the
  /// first characteristic of the named service that can do the job.
  private static func characteristic(
    in service: GATTServiceDescription, named: GATTUUID?,
    requiring required: GATTCharacteristicProperties
  ) -> GATTCharacteristicDescription? {
    if let named {
      return characteristic(in: service, anyOf: [named], requiring: required)
    }
    return service.characteristics.first { !$0.properties.isDisjoint(with: required) }
  }

  private static func selection(
    identity: BLEUARTProfile.Identity, service: GATTUUID,
    outbound: GATTCharacteristicDescription, inbound: GATTCharacteristicDescription,
    configuration: BLEEndpointConfiguration
  ) -> BLEEndpointSelection {
    BLEEndpointSelection(
      profile: identity,
      service: service,
      outbound: outbound.uuid,
      inbound: inbound.uuid,
      writeMode: writeMode(for: outbound.properties, configuration: configuration),
      inboundDelivery: inbound.properties.contains(.notify) ? .notify : .indicate)
  }

  /// Only ever called with properties already known to carry outbound traffic, so the
  /// final fallthrough is Write Command by elimination rather than by hope.
  private static func writeMode(
    for properties: GATTCharacteristicProperties, configuration: BLEEndpointConfiguration
  ) -> BLEWriteMode {
    if configuration.prefersWriteWithoutResponse, properties.contains(.writeWithoutResponse) {
      return .withoutResponse
    }
    return properties.contains(.write) ? .withResponse : .withoutResponse
  }
}

// MARK: - Chunking

/// Splitting a payload across ATT writes — §4 step (4), "chunk by CoreBluetooth
/// `maximumWriteValueLength`, never a fixed 20 bytes".
///
/// 20 is the ATT_MTU-3 of a 23-byte default MTU, and it is wrong in both directions. Too
/// small, on any modern peripheral: an iPhone negotiates 185 or more, so a fixed 20 throws
/// away nine tenths of the throughput and turns a receipt into hundreds of round trips.
/// Too large, occasionally: a peripheral is entitled to a *smaller* usable length for
/// Write Requests than for Write Commands, and a stack that silently truncates an
/// oversized value is how half a ticket prints with no error anywhere.
///
/// The number is also per **write type**, which is why the caller passes it in rather than
/// this file assuming one: `maximumWriteValueLength(for:)` answers differently for
/// `.withResponse` and `.withoutResponse` on the same characteristic.
public enum BLEChunking {
  /// The byte ranges to write, in order.
  ///
  /// - Parameters:
  ///   - byteCount: how many bytes the payload has.
  ///   - maximumWriteLength: `maximumWriteValueLength(for:)` for the write type actually
  ///     chosen. Values below 1 are clamped to 1 — a stack reporting zero is broken, and
  ///     dividing by it would either loop forever or drop the job silently.
  public static func ranges(byteCount: Int, maximumWriteLength: Int) -> [Range<Int>] {
    guard byteCount > 0 else { return [] }
    let limit = max(1, maximumWriteLength)
    var ranges: [Range<Int>] = []
    ranges.reserveCapacity((byteCount + limit - 1) / limit)
    var start = 0
    while start < byteCount {
      let end = min(start + limit, byteCount)
      ranges.append(start..<end)
      start = end
    }
    return ranges
  }
}

// MARK: - Backpressure

/// The one-shot gate a writer parks on while the stack is full — §4 step (5), "honour
/// `canSendWriteWithoutResponse` backpressure".
///
/// A Write Command that is issued while `canSendWriteWithoutResponse` is `false` is not
/// queued and not reported: it is **dropped**, and the next thing that happens is a
/// receipt missing a line in the middle with every layer above reporting success. The
/// documented remedy is to stop and wait for `peripheralIsReady(toSendWriteWithoutResponse:)`.
///
/// Two things this deliberately is not. It is not a spin loop — re-reading the flag in a
/// tight `while` burns the core's worker thread on a printer that is merely busy, and on a
/// single-core device can starve the very delegate queue that would clear it. And it is
/// not a sleep: a fixed pause is either too long, and halves throughput on a healthy link,
/// or too short, and is a spin loop wearing a disguise.
///
/// It is one-shot by design. Readiness is consumed by the writer that woke on it, because
/// the stack's signal means "room for *a* write", not "room from now on". A signal that
/// arrives with nobody waiting is retained, so the wait that follows returns immediately
/// rather than blocking for a wakeup that has already happened — the lost-wakeup race, and
/// the reason this is a condition variable and not a flag.
public final class BLEWriteGate: @unchecked Sendable {
  private let condition = NSCondition()
  private var isOpen: Bool
  private var isInvalidated = false

  /// - Parameter open: whether a signal is already pending.
  public init(open: Bool = false) { isOpen = open }

  /// Records that the stack can take another write. Called from the delegate queue.
  public func open() {
    condition.lock()
    isOpen = true
    condition.signal()
    condition.unlock()
  }

  /// Blocks until the gate opens, the deadline passes, or the link goes away.
  ///
  /// - Returns: `true` when readiness was consumed, `false` on timeout or after
  ///   ``invalidate()``. `false` is a short write, which the transport reports honestly
  ///   rather than rounding up to the full count.
  @discardableResult
  public func waitUntilOpen(timeout: TimeInterval) -> Bool {
    condition.lock()
    defer { condition.unlock() }
    let deadline = Date().addingTimeInterval(timeout)
    while !isOpen && !isInvalidated {
      if !condition.wait(until: deadline) { return false }
    }
    guard !isInvalidated else { return false }
    isOpen = false
    return true
  }

  /// Releases every waiter with `false`, for good. Called when the link drops or closes,
  /// so a write in flight fails in milliseconds instead of parking the core's worker for
  /// the whole timeout on a peripheral that is no longer there.
  public func invalidate() {
    condition.lock()
    isInvalidated = true
    condition.broadcast()
    condition.unlock()
  }

  /// Returns the gate to its initial state for a fresh connection. The core builds a new
  /// internal transport after a link drop and calls `open(link:)` again on the same
  /// object, so an invalidated gate has to be re-usable or the reconnection writes nothing.
  public func reset(open: Bool = false) {
    condition.lock()
    isOpen = open
    isInvalidated = false
    condition.unlock()
  }
}
