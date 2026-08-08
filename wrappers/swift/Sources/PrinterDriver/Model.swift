import CPrinterDriver
import Foundation

// MARK: - Errors

/// The reason a `pd_*` call returned nothing, taken verbatim from `pd_last_error`.
///
/// The ABI has one error channel and no error codes: a call either produces a handle or
/// leaves a message on the driver. This type carries that message and nothing else,
/// because inventing a code taxonomy here would be the wrapper deciding something.
public struct PrinterDriverError: Error, Hashable, Sendable, CustomStringConvertible {
  /// The driver's own words. Never empty.
  public let message: String

  init(_ message: String) {
    self.message = message.isEmpty ? "the driver reported no reason" : message
  }

  public var description: String { message }
}

// MARK: - Transport

/// How to reach a printer.
///
/// Only TCP exists in `pd.h` today. The other transports named in docs/api.md §2
/// (Bluetooth, USB, serial) are core work, not wrapper work: when `pd_add_printer_*`
/// grows a case, this enum grows the matching one.
public enum Transport: Hashable, Sendable {
  /// A raw-socket printer. Port 9100 is the near-universal ESC/POS default.
  case tcp(host: String, port: UInt16 = 9100)
}

// MARK: - Width

/// A printer's printable width in dots.
///
/// 384, 504 and 576 are the deployed widths; the ABI accepts any value, so this is a
/// `RawRepresentable` rather than a closed enum.
public struct PrinterWidth: RawRepresentable, Hashable, Sendable {
  public let rawValue: UInt32

  public init(rawValue: UInt32) { self.rawValue = rawValue }

  /// 58 mm paper.
  public static let dots384 = PrinterWidth(rawValue: 384)
  /// 76 mm paper.
  public static let dots504 = PrinterWidth(rawValue: 504)
  /// 80 mm paper — the ABI's default when a width of 0 is passed.
  public static let dots576 = PrinterWidth(rawValue: 576)
}

// MARK: - Job options

/// Everything optional about a submission — a 1:1 mirror of `pd_job_options`.
///
/// The zero value of every field is its documented default, so `JobOptions()` is the
/// safe configuration: profile cut, no drawer, strict preflight, the profile's own
/// completion timeout.
public struct JobOptions: Hashable, Sendable {
  /// The idempotency key: a caller-supplied stable id such as an order or ticket UUID.
  ///
  /// Re-submitting a key that already has a job **does not print** — ``Printer/print(_:options:)``
  /// returns that job's handle in whatever state it is in. `nil` lets the core generate a
  /// key, which keeps feedback working but gives up dedupe protection across restarts.
  /// Point-of-sale integrations should always pass one.
  public var key: String?

  /// How the job ends the paper. Defaults to whatever the printer's profile does.
  public var cut: Cut

  /// Pulse the cash drawer as part of this job.
  public var openDrawer: Bool

  /// `.strict` refuses to print onto a device already reporting cover-open or paper-out.
  public var preflight: Preflight

  /// Per-phase completion-wait budget. 0 uses the profile's own timeout.
  ///
  /// - Important: Exhausting this budget ends the job in ``JobResult/unknown(confidence:)``,
  ///   never in a failure.
  public var timeoutMilliseconds: UInt32

  /// Blank paper fed before the first content line — tear-off clearance, presentation
  /// space (docs/receipt-dsl.md "Margins").
  public var topFeedDots: UInt32

  /// The *total* whitespace between the last content and the cut.
  ///
  /// The core feeds `max(the profile's blade clearance, this)`, so it can only ever add
  /// paper: a value below the hardware minimum is silently ignored rather than allowed
  /// to slice through a trailing QR.
  public var bottomFeedDots: UInt32

  /// Print the receipt verification identifier in the ticket trailer — the `ORDER:` line
  /// and the trailer QR (docs/api.md §14). On by default.
  ///
  /// Turning it off suppresses the ink, not the evidence: the token is still journaled
  /// and ``PrinterDriver/job(token:)`` still resolves it.
  public var printsVerificationID: Bool

  public init(
    key: String? = nil,
    cut: Cut = .profile,
    openDrawer: Bool = false,
    preflight: Preflight = .strict,
    timeoutMilliseconds: UInt32 = 0,
    topFeedDots: UInt32 = 0,
    bottomFeedDots: UInt32 = 0,
    printsVerificationID: Bool = true
  ) {
    self.key = key
    self.cut = cut
    self.openDrawer = openDrawer
    self.preflight = preflight
    self.timeoutMilliseconds = timeoutMilliseconds
    self.topFeedDots = topFeedDots
    self.bottomFeedDots = bottomFeedDots
    self.printsVerificationID = printsVerificationID
  }

  /// Runs `body` with a C view of these options. The `key` buffer is valid only for the
  /// duration of the call, which is all the ABI needs: it copies every string it is given.
  func withABI<R>(_ body: (UnsafePointer<pd_job_options>) throws -> R) rethrows -> R {
    func build(_ keyPointer: UnsafePointer<CChar>?) throws -> R {
      var options = cValue(key: keyPointer)
      return try withUnsafePointer(to: &options) { try body($0) }
    }

    if let key {
      return try key.withCString { try build($0) }
    }
    return try build(nil)
  }

  /// The C struct for these options, borrowing `key`. Split out so ``ReprintOptions``
  /// can nest it without duplicating the field mapping.
  func cValue(key keyPointer: UnsafePointer<CChar>?) -> pd_job_options {
    pd_job_options(
      key: keyPointer,
      cut: pd_cut(cut.rawValue),
      open_drawer: openDrawer ? 1 : 0,
      preflight: pd_preflight(preflight.rawValue),
      timeout_ms: timeoutMilliseconds,
      top_feed_dots: topFeedDots,
      bottom_feed_dots: bottomFeedDots,
      // Inverted in the ABI so that an all-zeroes struct still prints the evidence.
      suppress_verification_id: printsVerificationID ? 0 : 1
    )
  }
}

// MARK: - Reprint options

/// Everything optional about a deliberate duplicate — a 1:1 mirror of
/// `pd_reprint_options`.
public struct ReprintOptions: Hashable, Sendable {
  /// The submission options the duplicate is printed with.
  public var job: JobOptions

  /// Print `*** REPRINT / POSSIBLE DUPLICATE ***` and the attempt counter. On by
  /// default.
  ///
  /// Turning it off is a per-call, deliberate act for a receipt where the banner is
  /// inappropriate — a customer-facing copy. A kitchen ticket should never turn it off:
  /// the banner is what lets staff bin the duplicate instead of cooking it twice.
  public var banner: Bool

  public init(job: JobOptions = JobOptions(), banner: Bool = true) {
    self.job = job
    self.banner = banner
  }

  /// Convenience for the common case of "the same options, minus the banner".
  public init(_ job: JobOptions, banner: Bool = true) {
    self.init(job: job, banner: banner)
  }

  func withABI<R>(_ body: (UnsafePointer<pd_reprint_options>) throws -> R) rethrows -> R {
    func build(_ keyPointer: UnsafePointer<CChar>?) throws -> R {
      var options = pd_reprint_options(
        job: job.cValue(key: keyPointer), suppress_banner: banner ? 0 : 1)
      return try withUnsafePointer(to: &options) { try body($0) }
    }

    if let key = job.key {
      return try key.withCString { try build($0) }
    }
    return try build(nil)
  }
}

// MARK: - Job event

/// One step of a job's history — a mirror of `pd_job_event`.
public struct JobEvent: Hashable, Sendable {
  /// The state the job moved into.
  public let state: JobState

  /// What evidence backs the claim as of this step.
  public let confidence: ConfidenceLevel

  /// Non-`nil` only on failure transitions. The ABI's `has_reason` flag is folded into
  /// this optional, so `.none` as a *value* can never be confused with "no reason given".
  public let reason: FailureReason?

  /// Milliseconds on a steady clock. The wall clock may step; job timing must not, so
  /// these are only ever meaningful as differences.
  public let monotonicMilliseconds: UInt64

  init(_ event: pd_job_event) {
    state = JobState(bridging: event.state.rawValue)
    confidence = ConfidenceLevel(bridging: event.confidence.rawValue)
    reason = event.has_reason != 0 ? FailureReason(bridging: event.reason.rawValue) : nil
    monotonicMilliseconds = event.monotonic_ms
  }
}

// MARK: - Job result

/// The terminal answer for a job — the tri-state of docs/api.md §1.4 and §4.
///
/// There is deliberately **no `isSuccess` boolean** anywhere in this package. Collapsing
/// ``unknown(confidence:)`` into either of the other two is exactly the bug that produces
/// duplicate kitchen tickets, so the compiler is made to force the three-way `switch`.
///
/// Every case carries a ``ConfidenceLevel``, including the failures: on a failure or an
/// unknown it records how far up the evidence ladder the job got before it stopped, which
/// is what an operator needs in order to decide about a reprint.
public enum JobResult: Hashable, Sendable {
  /// The job reached `doneSoftware` (or, later, `physicallyVerified`).
  ///
  /// - Parameters:
  ///   - confidence: what that claim rests on — `cutFaultFree` on a `GS ( H` printer,
  ///     `transportAccepted` on a write-only one. Never inflated.
  ///   - grade: what *class* of evidence it is, straight from the core.
  ///   - authority: who made the claim — the printer itself, a spooler, or nobody.
  ///   - method: the command behind it, e.g. `GS(H) fn48`. The string a support
  ///     engineer needs six months later; `"none"` when nothing was confirmed.
  case done(
    confidence: ConfidenceLevel, grade: ConfidenceGrade, authority: CompletionAuthority,
    method: String)

  /// Nothing printed, or the failure is confirmed: preflight refusal, transport
  /// unreachable, cutter fault. Safe to resubmit under the same key.
  case failed(reason: FailureReason, confidence: ConfidenceLevel)

  /// Bytes were sent and no acknowledgement came back — a completion timeout, a dropped
  /// link, a crash. **Not** a success and **not** a failure.
  ///
  /// Surface it to an operator and resolve it with ``Printer/forceReprint(key:options:)``
  /// or a manual confirmation. The SDK never retries out of this state on its own.
  case unknown(confidence: ConfidenceLevel)

  init(_ result: pd_job_result) {
    let confidence = ConfidenceLevel(bridging: result.confidence.rawValue)
    switch JobOutcome(bridging: result.outcome.rawValue) {
    case .done:
      self = .done(
        confidence: confidence,
        grade: ConfidenceGrade(bridging: result.grade.rawValue),
        authority: CompletionAuthority(bridging: result.authority.rawValue),
        method: result.method.map(String.init(cString:)) ?? "none")
    case .failed:
      self = .failed(
        reason: FailureReason(bridging: result.reason.rawValue), confidence: confidence)
    case .unknown:
      self = .unknown(confidence: confidence)
    }
  }

  /// The ABI discriminant behind this case.
  public var outcome: JobOutcome {
    switch self {
    case .done: return .done
    case .failed: return .failed
    case .unknown: return .unknown
    }
  }

  /// How far up the evidence ladder the job got, whichever way it ended.
  public var confidence: ConfidenceLevel {
    switch self {
    case .done(let confidence, _, _, _): return confidence
    case .failed(_, let confidence): return confidence
    case .unknown(let confidence): return confidence
    }
  }
}

// MARK: - Device status

/// A printer's last known state — a mirror of `pd_device_status`.
///
/// Never a live query, so reading it cannot block behind a print. The optional fields are
/// the ABI's tri-state: `nil` is `PD_UNKNOWN` and means *not observed*, which is not the
/// same as `false`. A snapshot that has never heard from the device says so through
/// ``hasObservedStatus`` rather than reporting healthy.
public struct DeviceStatus: Hashable, Sendable {
  /// A transport connection exists right now.
  public let isConnected: Bool

  /// At least one status frame has been decoded from this device. While `false`, every
  /// tri-state below is `nil`.
  public let hasObservedStatus: Bool

  public let isOnline: Bool?
  public let isCoverOpen: Bool?
  public let isPaperOut: Bool?
  public let isPaperNearEnd: Bool?
  public let hasCutterError: Bool?
  public let hasUnrecoverableError: Bool?
  public let hasRecoverableError: Bool?

  init(_ status: pd_device_status) {
    isConnected = status.connected == PD_TRUE
    hasObservedStatus = status.observed == PD_TRUE
    isOnline = DeviceStatus.triState(status.online)
    isCoverOpen = DeviceStatus.triState(status.cover_open)
    isPaperOut = DeviceStatus.triState(status.paper_out)
    isPaperNearEnd = DeviceStatus.triState(status.paper_near_end)
    hasCutterError = DeviceStatus.triState(status.cutter_error)
    hasUnrecoverableError = DeviceStatus.triState(status.unrecoverable_error)
    hasRecoverableError = DeviceStatus.triState(status.recoverable_error)
  }

  private static func triState(_ value: Int32) -> Bool? {
    switch value {
    case PD_TRUE: return true
    case PD_FALSE: return false
    default: return nil
    }
  }
}
