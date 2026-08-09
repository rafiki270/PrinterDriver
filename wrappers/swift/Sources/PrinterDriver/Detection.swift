import CPrinterDriver
import Foundation

// M15 — self-test, auto-detection and LAN discovery (docs/api.md §15).
//
// The wrapper contains no detection logic. Which provenance column governs a mechanism,
// what a printless probe may claim, how a ticket is laid out — all of it is in the core,
// behind `pd_self_test`, `pd_auto_detect` and `pd_discover`. What is here is the shape
// Swift callers expect: value types, `async`, and an `AsyncStream` for the two calls that
// deliver candidates as they are found.

// MARK: - Enums

/// How the capability profile in force was arrived at — `pd_profile_selection`.
///
/// Not a ``Provenance``: that says where a claim about one *capability* comes from, and
/// this says where the *profile* came from. A profile selected by documentation can still
/// have a single capability promoted by a probe.
public enum ProfileSelection: UInt32, ABIMirroredEnum {
  /// A device-database entry matched what the device reported about itself.
  case documented = 0
  /// A probe's first-hand findings promoted whatever was selected.
  case probed = 1
  /// Neither: the shipped default is the whole truth, which per
  /// docs/capability-profiles.md §8 means UNKNOWN DEVICE rather than ordinary device.
  case `default` = 2

  public static let abiTypeName = "ProfileSelection"
  /// Claims the least: an unrecognized selection has established nothing.
  public static let unrecognizedFallback = ProfileSelection.default

  /// The core's own spelling, from `pd_profile_selection_name`.
  public var abiName: String {
    String(cString: pd_profile_selection_name(pd_profile_selection(rawValue)))
  }
}

/// What auto-detection established about one address — `pd_detection_status`.
public enum DetectionStatus: UInt32, ABIMirroredEnum {
  /// The backchannel answered: identification, fences, or both.
  case answered = 0
  /// The port accepted the connection and said nothing at all. A real finding — the
  /// interface that does not forward status bytes — and never a failure.
  case silent = 1
  /// Reachable and deliberately not interrogated. Never render this as "no
  /// capabilities": nobody asked.
  case unverified = 2
  case unreachable = 3

  public static let abiTypeName = "DetectionStatus"
  /// Claims the least: an unrecognized status has reached nothing.
  public static let unrecognizedFallback = DetectionStatus.unreachable

  /// The core's own spelling, from `pd_detection_status_name`.
  public var abiName: String {
    String(cString: pd_detection_status_name(pd_detection_status(rawValue)))
  }
}

// MARK: - The detection report

/// What a device said about itself, and what that is worth.
///
/// The whole struct is evidence, never truth. ``isTrusted`` is false until a signal
/// independent of `GS I` agrees with `GS I`, because Rongta's own manual documents its
/// printers answering `"EPOSN"` / `"TM-T88V"`.
public struct DetectedIdentity: Hashable, Sendable {
  public let vendor: String
  public let model: String
  public let firmware: String
  public let serial: String
  public let isTrusted: Bool
  public let confidencePercent: UInt8
  public let isImpersonationSuspected: Bool
  /// Whether this identification came from the call that produced it rather than from
  /// the findings cache.
  public let isFresh: Bool
}

/// The media facts the renderer consumes. Roll width and raster width are separate facts
/// and neither is derived from the other: a 112 mm-media printer prints 104 mm.
public struct DetectedMedia: Hashable, Sendable {
  public let nominalPaperMillimetres: UInt16
  public let printableWidthDots: UInt32
  public let charactersPerLine: UInt32
  public let dotsPerInch: UInt16
}

/// The completion facts: the mechanism, the best grade a job on it can ever claim, who is
/// making that claim, and what the claim rests on.
public struct DetectedCompletion: Hashable, Sendable {
  public let mechanism: CompletionMechanism
  public let gradeCeiling: ConfidenceGrade
  public let authority: CompletionAuthority
  /// The command a support engineer looks up six months later, e.g. `"GS(H) fn48"`.
  public let method: String
  public let provenance: Provenance
}

/// The drawer facet, classified rather than fired (docs/cash-drawer.md).
public struct DetectedDrawer: Hashable, Sendable {
  public let isPresent: Bool
  /// Whether this engine may put a pulse on the wire: a drivable method AND an
  /// established electrical standard.
  public let isKickable: Bool
  public let portStandard: DrawerPortStandard
  public let voltage: UInt16
  public let electricalProvenance: Provenance
  public let commandsProvenance: Provenance
}

/// The detection report — `pd_detection_summary`.
///
/// The same structure the self-test prints on paper and auto-detection returns per
/// candidate, so the paper, the CLI, the agent and this wrapper describe a device the
/// same way.
public struct DetectionSummary: Hashable, Sendable {
  public let endpoint: String
  public let identity: DetectedIdentity
  public let profileID: String
  public let selection: ProfileSelection
  public let media: DetectedMedia
  public let completion: DetectedCompletion
  public let drawer: DetectedDrawer

  /// Everything that was requested and not delivered, in the words it is printed in —
  /// `"BARCODE not supported on this path"` and its relatives. Empty when nothing was
  /// dropped.
  public let degradations: [String]

  /// One line for a table row, e.g.
  /// `"GS(H) fn48 Probed - profile Probed - identity untrusted (35%)"`.
  public let provenanceSummary: String

  init(_ wire: pd_detection_summary) {
    endpoint = String(cString: wire.endpoint)
    identity = DetectedIdentity(
      vendor: String(cString: wire.vendor),
      model: String(cString: wire.model),
      firmware: String(cString: wire.firmware),
      serial: String(cString: wire.serial),
      isTrusted: wire.identity_trusted != 0,
      confidencePercent: wire.confidence_percent,
      isImpersonationSuspected: wire.impersonation_suspected != 0,
      isFresh: wire.identity_fresh != 0)
    profileID = String(cString: wire.profile_id)
    selection = ProfileSelection(bridging: wire.selection.rawValue)
    media = DetectedMedia(
      nominalPaperMillimetres: wire.nominal_paper_mm,
      printableWidthDots: wire.printable_width_dots,
      charactersPerLine: wire.chars_per_line,
      dotsPerInch: wire.dpi)
    completion = DetectedCompletion(
      mechanism: CompletionMechanism(bridging: wire.completion.rawValue),
      gradeCeiling: ConfidenceGrade(bridging: wire.grade_ceiling.rawValue),
      authority: CompletionAuthority(bridging: wire.authority.rawValue),
      method: String(cString: wire.method),
      provenance: Provenance(bridging: wire.completion_provenance.rawValue))
    drawer = DetectedDrawer(
      isPresent: wire.drawer_present != 0,
      isKickable: wire.drawer_kickable != 0,
      portStandard: DrawerPortStandard(bridging: wire.drawer_standard.rawValue),
      voltage: wire.drawer_voltage,
      electricalProvenance: Provenance(bridging: wire.drawer_electrical_provenance.rawValue),
      commandsProvenance: Provenance(bridging: wire.drawer_commands_provenance.rawValue))
    provenanceSummary = String(cString: wire.provenance_summary)

    var lines: [String] = []
    if let base = wire.degradations {
      for index in 0..<Int(wire.degradation_count) {
        if let entry = base[index] {
          lines.append(String(cString: entry))
        }
      }
    }
    degradations = lines
  }
}

// MARK: - Self-test

/// Options for ``Printer/selfTest(_:)``. The useful call is `selfTest()`.
public struct SelfTestOptions: Hashable, Sendable {
  /// `nil` produces `"selftest-<unix ms>"`. A real idempotency key on a real job: the
  /// same key twice does not print twice.
  public var key: String?
  /// Interrogate the device now instead of using what is already known. Runs the same
  /// probe `addPrinter` schedules, on the printer's own worker thread.
  public var refreshIdentity: Bool
  /// Keep that refresh printless, at the cost of asking the ordered fences out of an
  /// empty buffer.
  public var probeWithoutPrinting: Bool
  /// Include the Code 128 sample. A profile with no barcode path omits it anyway, with a
  /// declared degradation printed on the ticket.
  public var barcode: Bool
  public var barcodeData: String?
  /// Print the trailer `V:` line and its QR. On a `GS ( H` printer that QR carries the
  /// job's own verification token, which is what makes the paper evidence.
  public var printVerificationID: Bool
  /// 0 means the profile's completion budget.
  public var timeoutMilliseconds: UInt32

  public init(
    key: String? = nil,
    refreshIdentity: Bool = false,
    probeWithoutPrinting: Bool = false,
    barcode: Bool = true,
    barcodeData: String? = nil,
    printVerificationID: Bool = true,
    timeoutMilliseconds: UInt32 = 0
  ) {
    self.key = key
    self.refreshIdentity = refreshIdentity
    self.probeWithoutPrinting = probeWithoutPrinting
    self.barcode = barcode
    self.barcodeData = barcodeData
    self.printVerificationID = printVerificationID
    self.timeoutMilliseconds = timeoutMilliseconds
  }
}

/// What one diagnostic ticket established.
///
/// ``result`` is the proof: it is the ordinary tri-state outcome of the ordinary engine,
/// so a `.done` at ``ConfidenceGrade/aJobLevelConfirmation`` is the statement that this
/// stack works end to end on this unit, over this interface path.
public struct SelfTestResult: Sendable {
  public let result: JobResult
  public let detection: DetectionSummary
  public let key: String
  /// The four `GS ( H` characters printed as `V:` and inside the QR. `nil` on a profile
  /// with no wire token to promote.
  public let verificationID: String?
  /// The ticket exactly as it was laid out, one entry per line — the same layout that
  /// produced the bytes, never a second one.
  public let ticketLines: [String]
  /// The ordinary job handle, so the ticket can be resolved by key or by token later.
  public let job: PrintJob?
}

extension Printer {
  /// Prints ONE diagnostic ticket through the full fenced engine and returns what it
  /// established. **This uses paper: the paper is the report.**
  ///
  /// Identity, profile and how it was selected, media, completion mechanism with its
  /// grade ceiling and provenance, the drawer classification, a Czech/Hungarian/Polish
  /// charset line, a Code 128 sample and the job's own verification token in the trailer
  /// QR. Anything the profile cannot draw is printed as a declared degradation rather
  /// than dropped, and repeated in ``DetectionSummary/degradations``.
  ///
  /// The printer's OWN built-in self-test (`GS ( A`) is a different document, and stays
  /// separately reachable through `pdctl test-print`: vendor firmware's view of the
  /// device against this, the SDK's.
  ///
  /// - Throws: ``PrinterDriverError`` when the handles do not match.
  public func selfTest(_ options: SelfTestOptions = SelfTestOptions()) throws
    -> SelfTestResult
  {
    var wire = pd_self_test_options()
    wire.refresh_identity = options.refreshIdentity ? 1 : 0
    wire.probe_without_printing = options.probeWithoutPrinting ? 1 : 0
    wire.no_barcode = options.barcode ? 0 : 1
    wire.no_verification_id = options.printVerificationID ? 0 : 1
    wire.timeout_ms = options.timeoutMilliseconds

    var out = pd_self_test_result()
    let ok = withOptionalCString(options.key) { key in
      wire.key = key
      return withOptionalCString(options.barcodeData) { data in
        wire.barcode_data = data
        return withUnsafePointer(to: &wire) { optionsPointer in
          pd_self_test(core.handle, handle, optionsPointer, &out)
        }
      }
    }
    guard ok != 0 else { throw core.lastError() }

    let job = out.job.map { core.internJob($0, mechanism: completionMechanism) }
    let token = String(cString: out.print_token)
    let ticket = String(cString: out.ticket_text)
    return SelfTestResult(
      result: JobResult(out.result),
      detection: DetectionSummary(out.detection),
      key: String(cString: out.key),
      verificationID: token.isEmpty ? nil : token,
      ticketLines: ticket.isEmpty
        ? [] : ticket.split(separator: "\n", omittingEmptySubsequences: false).map(String.init),
      job: job)
  }

  /// Runs `selfTest(_:)` off the calling thread.
  public func selfTest(_ options: SelfTestOptions = SelfTestOptions()) async throws
    -> SelfTestResult
  {
    let printer = self
    return try await withCheckedThrowingContinuation { continuation in
      DispatchQueue.global(qos: .userInitiated).async {
        do {
          continuation.resume(returning: try printer.selfTest(options) as SelfTestResult)
        } catch {
          continuation.resume(throwing: error)
        }
      }
    }
  }
}

// MARK: - Auto-detection

/// Options for ``PrinterDriver/autoDetect(_:)``.
public struct AutoDetectOptions: Hashable, Sendable {
  /// `nil` sweeps the local /24. Ignored when ``endpoints`` is non-empty.
  public var subnetCIDR: String?
  /// An explicit candidate list, `"host"` or `"host:port"`. Skips the sweep entirely —
  /// the path for a caller with a known inventory, and the only one that reports an
  /// unreachable address, because it is the only one where somebody named it.
  public var endpoints: [String]
  public var port: UInt16
  public var concurrency: UInt32
  public var connectTimeoutMilliseconds: UInt32
  public var responseTimeoutMilliseconds: UInt32
  /// False leaves devices nobody has interrogated alone: cached findings still apply and
  /// anything untouched comes back ``DetectionStatus/unverified``.
  public var probeUnknown: Bool

  public init(
    subnetCIDR: String? = nil,
    endpoints: [String] = [],
    port: UInt16 = 0,
    concurrency: UInt32 = 0,
    connectTimeoutMilliseconds: UInt32 = 0,
    responseTimeoutMilliseconds: UInt32 = 0,
    probeUnknown: Bool = true
  ) {
    self.subnetCIDR = subnetCIDR
    self.endpoints = endpoints
    self.port = port
    self.concurrency = concurrency
    self.connectTimeoutMilliseconds = connectTimeoutMilliseconds
    self.responseTimeoutMilliseconds = responseTimeoutMilliseconds
    self.probeUnknown = probeUnknown
  }
}

/// One candidate, classified — `pd_detected_printer`.
public struct DetectedPrinter: Hashable, Sendable {
  public let endpoint: String
  public let host: String
  public let port: UInt16
  public let status: DetectionStatus
  public let isPortOpen: Bool
  /// True when the classification came from stored findings rather than from bytes
  /// exchanged in this call.
  public let isFromCache: Bool
  /// Whatever `DLE EOT 1` answered during the sweep, as uppercase hex. Empty when the
  /// port accepted the connection and said nothing.
  public let dleEotHex: String
  public let summary: DetectionSummary

  init(_ wire: pd_detected_printer) {
    endpoint = String(cString: wire.endpoint)
    host = String(cString: wire.host)
    port = wire.port
    status = DetectionStatus(bridging: wire.status.rawValue)
    isPortOpen = wire.port_open != 0
    isFromCache = wire.from_cache != 0
    dleEotHex = String(cString: wire.dle_eot_hex)
    summary = DetectionSummary(wire.summary)
  }
}

/// One address the LAN sweep found listening — `pd_discovered_device`.
public struct DiscoveredDevice: Hashable, Sendable {
  public let ip: String
  public let port: UInt16
  public let isPortOpen: Bool
  /// `DLE EOT 1`'s answer as uppercase hex, verbatim and unclassified. Empty means the
  /// port accepted the connection and said nothing — a LAN module that does not forward
  /// status bytes, which is a finding and not a failure.
  public let dleEotHex: String

  /// Whether anything came back on the backchannel at all.
  public var didAnswer: Bool { !dleEotHex.isEmpty }

  init(_ wire: pd_discovered_device) {
    ip = String(cString: wire.ip)
    port = wire.port
    isPortOpen = wire.port9100_open != 0
    dleEotHex = String(cString: wire.dle_eot_hex)
  }
}

/// Options for ``PrinterDriver/discover(_:)``.
public struct DiscoverOptions: Hashable, Sendable {
  /// `nil` sweeps the local /24.
  public var subnetCIDR: String?
  public var port: UInt16
  public var concurrency: UInt32
  public var connectTimeoutMilliseconds: UInt32
  public var responseTimeoutMilliseconds: UInt32
  /// False turns the sweep into a pure port scan: the port state is still reported and
  /// not one byte is written.
  public var probeBackchannel: Bool

  public init(
    subnetCIDR: String? = nil,
    port: UInt16 = 0,
    concurrency: UInt32 = 0,
    connectTimeoutMilliseconds: UInt32 = 0,
    responseTimeoutMilliseconds: UInt32 = 0,
    probeBackchannel: Bool = true
  ) {
    self.subnetCIDR = subnetCIDR
    self.port = port
    self.concurrency = concurrency
    self.connectTimeoutMilliseconds = connectTimeoutMilliseconds
    self.responseTimeoutMilliseconds = responseTimeoutMilliseconds
    self.probeBackchannel = probeBackchannel
  }
}

// The ABI's callbacks are C function pointers with a void* context, so the Swift closure
// has to be reachable through that pointer. A final class, passed unretained for the
// duration of a call this thread is blocked inside, is the whole mechanism.
private final class DetectSink {
  let onCandidate: (DetectedPrinter) -> Void
  init(_ onCandidate: @escaping (DetectedPrinter) -> Void) { self.onCandidate = onCandidate }
}

private final class DiscoverSink {
  let onDevice: (DiscoveredDevice) -> Void
  init(_ onDevice: @escaping (DiscoveredDevice) -> Void) { self.onDevice = onDevice }
}

extension PrinterDriver {
  /// Sweeps, identifies and classifies. **Nothing prints and nothing fires.**
  ///
  /// Discovery (the non-printing `DLE EOT 1` sweep) → multi-signal identification per
  /// candidate → the PRINTLESS subset of the capability probe, respecting the stored
  /// findings cache.
  ///
  /// - Important: an ordered fence only means anything when there is print data ahead of
  ///   it. This call has none, so the fences go out behind an empty buffer: a device that
  ///   echoes them has proved that its firmware *implements* the command, not that the
  ///   echo waits for paper to move. The flag is promoted and its provenance is not —
  ///   ``DetectedCompletion/provenance`` stays ``Provenance/unverified`` on a printless
  ///   answer, and the reason appears in ``DetectionSummary/degradations``. Full
  ///   promotion needs the printing probe or a real job.
  ///
  /// - Returns: every candidate examined. Blocks; prefer ``autoDetectStream(_:)``.
  /// - Throws: ``PrinterDriverError`` on a malformed CIDR, or one wider than /16.
  public func autoDetect(
    _ options: AutoDetectOptions = AutoDetectOptions(),
    onCandidate: (@Sendable (DetectedPrinter) -> Void)? = nil
  ) throws -> [DetectedPrinter] {
    var found: [DetectedPrinter] = []
    let lock = NSLock()
    let sink = DetectSink { printer in
      lock.lock()
      found.append(printer)
      lock.unlock()
      onCandidate?(printer)
    }

    var wire = pd_auto_detect_options()
    wire.port = options.port
    wire.concurrency = options.concurrency
    wire.connect_timeout_ms = options.connectTimeoutMilliseconds
    wire.response_timeout_ms = options.responseTimeoutMilliseconds
    wire.leave_unknown_unprobed = options.probeUnknown ? 0 : 1

    let count = withOptionalCString(options.subnetCIDR) { cidr -> Int32 in
      wire.subnet_cidr = cidr
      return withCStringArray(options.endpoints) { endpoints -> Int32 in
        wire.endpoints = endpoints
        return withUnsafePointer(to: &wire) { optionsPointer in
          pd_auto_detect(
            core.handle, optionsPointer,
            { printer, _, _, ctx in
              guard let printer, let ctx else { return }
              Unmanaged<DetectSink>.fromOpaque(ctx).takeUnretainedValue()
                .onCandidate(DetectedPrinter(printer.pointee))
            }, Unmanaged.passUnretained(sink).toOpaque())
        }
      }
    }
    guard count >= 0 else { throw core.lastError() }
    return found
  }

  /// ``autoDetect(_:onCandidate:)`` as a stream: candidates arrive as they are found and
  /// the stream finishes when the sweep does.
  public func autoDetectStream(_ options: AutoDetectOptions = AutoDetectOptions())
    -> AsyncThrowingStream<DetectedPrinter, Error>
  {
    let driver = self
    return AsyncThrowingStream { continuation in
      DispatchQueue.global(qos: .userInitiated).async {
        do {
          _ = try driver.autoDetect(options) { continuation.yield($0) }
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
    }
  }

  /// The raw sweep underneath ``autoDetect(_:onCandidate:)``: is something ESC/POS-shaped
  /// listening, and does its backchannel reach me?
  ///
  /// No identification, no capability probe, no profile — deciding what a device *is*
  /// costs time and belongs to auto-detection. The whole write side is `DLE EOT 1`
  /// (`10 04 01`), every byte of which is below 0x20 and therefore cannot print on any
  /// device, ever.
  ///
  /// - Returns: the open ports, sorted by address. Blocks; prefer ``discoverStream(_:)``.
  /// - Throws: ``PrinterDriverError`` on a malformed CIDR, one wider than /16, or no
  ///   local subnet to guess.
  public func discover(
    _ options: DiscoverOptions = DiscoverOptions(),
    onDevice: (@Sendable (DiscoveredDevice) -> Void)? = nil
  ) throws -> [DiscoveredDevice] {
    var found: [DiscoveredDevice] = []
    let lock = NSLock()
    let sink = DiscoverSink { device in
      lock.lock()
      found.append(device)
      lock.unlock()
      onDevice?(device)
    }

    var wire = pd_discover_options()
    wire.port = options.port
    wire.concurrency = options.concurrency
    wire.connect_timeout_ms = options.connectTimeoutMilliseconds
    wire.response_timeout_ms = options.responseTimeoutMilliseconds
    wire.no_backchannel_probe = options.probeBackchannel ? 0 : 1

    let count = withOptionalCString(options.subnetCIDR) { cidr -> Int32 in
      wire.subnet_cidr = cidr
      return withUnsafePointer(to: &wire) { optionsPointer in
        pd_discover(
          core.handle, optionsPointer,
          { device, _, _, ctx in
            guard let device, let ctx else { return }
            Unmanaged<DiscoverSink>.fromOpaque(ctx).takeUnretainedValue()
              .onDevice(DiscoveredDevice(device.pointee))
          }, Unmanaged.passUnretained(sink).toOpaque())
      }
    }
    guard count >= 0 else { throw core.lastError() }
    return found
  }

  /// ``discover(_:onDevice:)`` as a stream: listeners arrive as they are found.
  public func discoverStream(_ options: DiscoverOptions = DiscoverOptions())
    -> AsyncThrowingStream<DiscoveredDevice, Error>
  {
    let driver = self
    return AsyncThrowingStream { continuation in
      DispatchQueue.global(qos: .userInitiated).async {
        do {
          _ = try driver.discover(options) { continuation.yield($0) }
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
    }
  }

  /// The /24 around this host's primary address, as `"192.168.1.0/24"`, or `nil` when it
  /// cannot be determined.
  ///
  /// Found by asking the routing table which local address would be used to reach a
  /// remote one; no packet is transmitted.
  public var localSubnet: String? {
    let value = String(cString: pd_local_subnet(core.handle))
    return value.isEmpty ? nil : value
  }
}

// MARK: - C string plumbing

/// Runs `body` with `value` as a NUL-terminated C string, or with `nil` when it is `nil`.
private func withOptionalCString<T>(
  _ value: String?, _ body: (UnsafePointer<CChar>?) -> T
) -> T {
  guard let value else { return body(nil) }
  return value.withCString { body($0) }
}

/// Runs `body` with a NULL-terminated `char*` array.
///
/// The strings are heap copies rather than pointers into Swift arrays: a pointer taken
/// from `[CChar]` inside a nested closure is only valid for that closure, and the ABI
/// needs the whole array alive for one call. pd.h copies every string it is passed before
/// returning, so freeing them here is safe and complete.
private func withCStringArray<T>(
  _ values: [String], _ body: (UnsafePointer<UnsafePointer<CChar>?>?) -> T
) -> T {
  guard !values.isEmpty else { return body(nil) }
  let owned: [UnsafeMutablePointer<CChar>?] = values.map { strdup($0) }
  defer { owned.forEach { free($0) } }
  var pointers: [UnsafePointer<CChar>?] = owned.map { pointer in
    pointer.map { UnsafePointer<CChar>($0) }
  }
  pointers.append(nil)
  return pointers.withUnsafeBufferPointer { body($0.baseAddress) }
}
