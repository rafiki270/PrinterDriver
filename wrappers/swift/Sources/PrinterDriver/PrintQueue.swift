import CPrinterDriver
import Foundation

// M13b. The print-queue addon (docs/sdk-spec.md §12), reached through `pd_queue_*`.
//
// Layered on the public API, never part of it. The core already contains the only queue
// correctness requires — one active job per printer, with a completion fence between
// jobs — and that part is not optional and not policy. Everything here is policy: holding
// while a printer is unusable, draining on recovery, expiry, priority, depth limits.
// Different venues want different answers, which is exactly what makes this an addon.
//
// Three rules from §12 are load-bearing, and this wrapper deliberately implements none of
// them: they live in the C++ addon behind the ABI, so a Swift caller gets byte-identical
// behaviour to a Kotlin one.
//
//  1. **A queue is not a retry engine.** A job that ends ``JobOutcome/unknown`` blocks its
//     printer's lane, and nothing further drains onto that printer until ``unblock(_:)``
//     is called by somebody who has looked at the paper. No timer clears it, because no
//     timer can see a receipt.
//  2. **Idempotency keys flow through.** Enqueuing a key that already has a job — held,
//     printing, or finished months ago — returns that job and prints nothing.
//  3. **No bypass.** Draining runs the identical engine path ``Printer/print(_:options:)``
//     takes: same worker, same preflight, same fences, same confidence grading.

/// In what order a lane's waiting jobs are chosen.
public enum DrainOrder: UInt32, Sendable {
  /// Submission order. The safe default for tickets that must stay in sequence.
  case fifo = 0
  /// Higher ``QueueOptions/priority`` first, submission order within equal priorities.
  case priority = 1

  /// The core's own spelling, from `pd_drain_order_name`.
  public var abiName: String {
    String(cString: pd_drain_order_name(pd_drain_order(rawValue)))
  }
}

/// What the queue does with a job it cannot send yet.
public struct QueuePolicy: Sendable {
  /// Park jobs while the printer is known to be offline, coverless or out of paper,
  /// instead of failing them one at a time. `false` makes the queue a pure serializer.
  public var holdWhileOffline: Bool
  /// Shelf life for a held job, in milliseconds; `0` means it never expires. A kitchen
  /// ticket must not print into a recovered kitchen half an hour late.
  public var defaultTimeToLiveMilliseconds: UInt32
  /// Held jobs per printer. `0` is unlimited, which recreates the printer's own buffer
  /// problem one layer up.
  public var maxDepth: UInt32
  public var drainOrder: DrainOrder

  public init(
    holdWhileOffline: Bool = true,
    defaultTimeToLiveMilliseconds: UInt32 = 0,
    maxDepth: UInt32 = 64,
    drainOrder: DrainOrder = .fifo
  ) {
    self.holdWhileOffline = holdWhileOffline
    self.defaultTimeToLiveMilliseconds = defaultTimeToLiveMilliseconds
    self.maxDepth = maxDepth
    self.drainOrder = drainOrder
  }

  var cValue: pd_queue_policy {
    pd_queue_policy(
      hold_while_offline: holdWhileOffline ? 1 : 0,
      default_ttl_ms: defaultTimeToLiveMilliseconds,
      max_depth: maxDepth,
      drain_order: pd_drain_order(drainOrder.rawValue))
  }
}

/// Per-job queue settings. The printing half mirrors ``JobOptions``.
public struct QueueOptions: Sendable {
  /// Idempotency key. `nil` generates one, and generated keys dedupe against nothing.
  public var key: String?
  /// `0` uses ``QueuePolicy/defaultTimeToLiveMilliseconds``.
  public var timeToLiveMilliseconds: UInt32
  /// Orders the waiting set only. A job already in flight is never preempted.
  public var priority: Int32
  public var cut: Cut
  public var openDrawer: Bool
  public var preflight: Preflight
  /// `0` uses the profile's completion timeout.
  public var timeoutMilliseconds: UInt32

  public init(
    key: String? = nil,
    timeToLiveMilliseconds: UInt32 = 0,
    priority: Int32 = 0,
    cut: Cut = .profile,
    openDrawer: Bool = false,
    preflight: Preflight = .strict,
    timeoutMilliseconds: UInt32 = 0
  ) {
    self.key = key
    self.timeToLiveMilliseconds = timeToLiveMilliseconds
    self.priority = priority
    self.cut = cut
    self.openDrawer = openDrawer
    self.preflight = preflight
    self.timeoutMilliseconds = timeoutMilliseconds
  }

  func withABI<R>(_ body: (UnsafePointer<pd_queue_options>) throws -> R) rethrows -> R {
    func build(_ keyPointer: UnsafePointer<CChar>?) throws -> R {
      var options = pd_queue_options(
        key: keyPointer,
        ttl_ms: timeToLiveMilliseconds,
        priority: priority,
        cut: pd_cut(cut.rawValue),
        open_drawer: openDrawer ? 1 : 0,
        preflight: pd_preflight(preflight.rawValue),
        timeout_ms: timeoutMilliseconds)
      return try withUnsafePointer(to: &options) { try body($0) }
    }
    if let key {
      return try key.withCString { try build($0) }
    }
    return try build(nil)
  }
}

/// A policy queue in front of one ``PrinterDriver``.
///
/// Holds the driver alive, and must be released before it — which the stored reference
/// takes care of.
public final class PrintQueue: @unchecked Sendable {
  private let driver: PrinterDriver
  private let core: DriverCore
  private let handle: OpaquePointer

  /// - Throws: ``PrinterDriverError`` when the driver has already been torn down.
  public init(driver: PrinterDriver, policy: QueuePolicy = QueuePolicy()) throws {
    var cPolicy = policy.cValue
    guard
      let handle = withUnsafePointer(to: &cPolicy, {
        pd_queue_create(driver.core.handle, $0)
      })
    else {
      throw driver.core.lastError()
    }
    self.driver = driver
    self.core = driver.core
    self.handle = handle
  }

  deinit {
    // Held jobs stay held and stay non-terminal: the queue does not invent an outcome for
    // a job whose fate it does not know.
    pd_queue_destroy(handle)
  }

  /// Enqueues a job.
  ///
  /// The returned ``PrintJob`` is an ordinary job handle — same id, same event stream —
  /// already sent when the printer is usable and its lane is free, otherwise in
  /// ``JobState/heldOffline``, or already terminal with ``FailureReason/queueOverflow``
  /// when the lane is full.
  @discardableResult
  public func enqueue(
    _ payload: Payload, to printer: Printer, options: QueueOptions = QueueOptions()
  ) throws -> PrintJob {
    let jobHandle = try payload.withABI { payloadPointer in
      try options.withABI { optionsPointer -> OpaquePointer in
        guard let job = pd_queue_enqueue(handle, printer.handle, payloadPointer, optionsPointer)
        else {
          throw core.lastError()
        }
        return job
      }
    }
    return core.internJob(jobHandle, mechanism: printer.completionMechanism)
  }

  /// Operator hold, independent of what the device is reporting.
  public func pause(_ printerID: String) { printerID.withCString { pd_queue_pause(handle, $0) } }
  public func resume(_ printerID: String) { printerID.withCString { pd_queue_resume(handle, $0) } }
  public func isPaused(_ printerID: String) -> Bool {
    printerID.withCString { pd_queue_is_paused(handle, $0) != 0 }
  }

  /// `true` once a job on this printer ended ``JobOutcome/unknown``. Rule 1: nothing more
  /// drains onto that lane until a person has looked at the paper and called
  /// ``unblock(_:)``.
  public func isBlocked(_ printerID: String) -> Bool {
    printerID.withCString { pd_queue_is_blocked(handle, $0) != 0 }
  }
  public func unblock(_ printerID: String) {
    printerID.withCString { pd_queue_unblock(handle, $0) }
  }

  /// Held jobs. Omit `printerID` to count every lane.
  public func pending(_ printerID: String? = nil) -> Int {
    guard let printerID else { return Int(pd_queue_pending(handle, nil)) }
    return printerID.withCString { Int(pd_queue_pending(handle, $0)) }
  }

  public var expiredCount: Int { Int(pd_queue_expired_count(handle)) }
  public var overflowCount: Int { Int(pd_queue_overflow_count(handle)) }
  public var drainedCount: Int { Int(pd_queue_drained_count(handle)) }

  /// Runs one expiry-and-drain pass on the calling thread. The queue's own thread already
  /// does this on every device event and whenever a TTL comes due.
  public func tick() { pd_queue_tick(handle) }
}
