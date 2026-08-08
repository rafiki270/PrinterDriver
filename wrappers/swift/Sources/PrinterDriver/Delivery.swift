import CPrinterDriver
import Foundation

// MARK: - Threading contract
//
// pd.h invokes its callbacks on the core's own threads: job events on the owning
// printer's worker thread (except the replay inside pd_subscribe_job, which runs on the
// calling thread), device events on whichever thread decoded the status. A callback there
// must not block and must not re-enter any pd_* function on the same driver — the worker
// it is running on is the thread that would have to service that call.
//
// Everything in this file exists to make that invisible to callers. A C callback does the
// least possible work: convert the struct to a Swift value and hand it to a private
// serial queue owned by the driver. All wrapper state lives on that queue, so:
//
//   * app code never runs on a core thread;
//   * app code may call straight back into the SDK from a handler;
//   * per-job ordering is preserved, because a serial queue cannot reorder;
//   * an AsyncStream and a closure handler on the same job observe the same order.
//
// The queue is not the main queue. UI work must hop itself — `await MainActor.run { … }`
// or a `Task { @MainActor in … }` — which is also why nothing here is annotated
// @MainActor: forcing the hop on every event would serialise printing behind the UI.

// MARK: - Driver core

/// The owner of the C driver handle and of the delivery queue.
///
/// Split out from ``PrinterDriver`` so that ``Printer`` and ``PrintJob`` can keep the
/// driver alive without a reference cycle through the public facade.
/// All mutable state is either confined to the delivery queue or guarded by `lock`,
/// which is what makes the unchecked conformance true rather than convenient.
final class DriverCore: @unchecked Sendable {
  let handle: OpaquePointer

  /// The one serial queue every callback, stream and completion handler is delivered on.
  let delivery: DispatchQueue

  // Two locks rather than one, and never nested: interning a job constructs a `PrintJob`,
  // whose initialiser registers a trampoline, so a single lock would deadlock on itself.
  private let trampolineLock = NSLock()
  private let jobLock = NSLock()

  /// Callback contexts. pd.h has no unsubscribe, so a context passed to the ABI must stay
  /// valid until `pd_destroy`; holding them here does exactly that, and holding them
  /// *only* here (they keep no strong references back) is what keeps the graph acyclic.
  private var trampolines: [AnyObject] = []

  /// Interned jobs, weakly. The ABI maps one underlying job to one stable `pd_job*`, so
  /// re-submitting an idempotency key can and should hand back the same Swift object;
  /// weakly, because a `PrintJob` holds this core and a strong table would be a cycle.
  private var jobs: [OpaquePointer: WeakJob] = [:]

  init(handle: OpaquePointer) {
    self.handle = handle
    delivery = DispatchQueue(
      label: "com.printerdriver.delivery", qos: .userInitiated, target: .global(qos: .userInitiated))
  }

  deinit {
    // Order matters and is the reverse of construction: pd_destroy stops every worker and
    // waits for in-flight jobs to reach a terminal state, so no callback can be running by
    // the time `trampolines` is released with the rest of this object.
    pd_destroy(handle)
  }

  /// The driver's own account of why the last call produced nothing.
  func lastError() -> PrinterDriverError {
    PrinterDriverError(String(cString: pd_last_error(handle)))
  }

  func retain(trampoline: AnyObject) {
    trampolineLock.lock()
    defer { trampolineLock.unlock() }
    trampolines.append(trampoline)
  }

  /// Returns the `PrintJob` already wrapping `pointer`, or wraps it now.
  func internJob(_ pointer: OpaquePointer, authority: CompletionAuthority) -> PrintJob {
    jobLock.lock()
    defer { jobLock.unlock() }
    if let existing = jobs[pointer]?.job {
      return existing
    }
    let job = PrintJob(core: self, handle: pointer, authority: authority)
    jobs[pointer] = WeakJob(job: job)
    return job
  }

  private struct WeakJob {
    weak var job: PrintJob?
  }
}

// MARK: - Trampolines

/// Carries job events from a core worker thread onto the delivery queue, in order.
///
/// Holds its hub weakly: the ABI cannot unsubscribe, so this object outlives the job
/// wrapper it feeds, and once nobody is listening the events simply stop being converted.
///
/// ### Why the subscription window exists
///
/// The core makes registration and replay atomic from the subscriber's point of view:
/// everything recorded up to `pd_subscribe_job`'s return is delivered on the calling
/// thread, in order, before the stream moves to the worker. (Cores before the
/// registration/replay fix in PrintJob::subscribe lacked that guarantee — a worker
/// emitting mid-subscribe could hand the callback a live event ahead of the older
/// recorded ones, observed as `bytesSent` arriving before `queued` — and this window
/// is the workaround that shipped against them.)
///
/// It is kept because it costs one lock and keeps the wrapper's ordering guarantee
/// independent of the core's: during the window the two sources are buffered apart by
/// thread identity — the ABI documents replay on the calling thread, everything else
/// on the worker — and flushed recorded-before-live. Both the buffering decision and
/// the hand-off to the delivery queue happen under one lock, so events reach the queue
/// in exactly the order the core produced them.
final class JobEventTrampoline: @unchecked Sendable {
  private let delivery: DispatchQueue
  private let lock = NSLock()
  private weak var hub: JobEventHub?

  /// Non-nil only while `pd_subscribe_job` is running, holding the thread that called it.
  private var subscribingThread: pthread_t?
  private var recorded: [JobEvent] = []
  private var live: [JobEvent] = []

  init(hub: JobEventHub, delivery: DispatchQueue) {
    self.hub = hub
    self.delivery = delivery
  }

  /// Opens the subscription window. Call immediately before `pd_subscribe_job`.
  func beginSubscription() {
    lock.lock()
    subscribingThread = pthread_self()
    lock.unlock()
  }

  /// Closes it and flushes what arrived, recorded first. Call immediately after.
  func endSubscription() {
    lock.lock()
    let ordered = recorded + live
    recorded.removeAll()
    live.removeAll()
    subscribingThread = nil
    enqueueLocked(ordered)
    lock.unlock()
  }

  fileprivate func deliver(_ event: pd_job_event) {
    let value = JobEvent(event)
    lock.lock()
    defer { lock.unlock() }
    if let subscribingThread {
      if pthread_equal(subscribingThread, pthread_self()) != 0 {
        recorded.append(value)
      } else {
        live.append(value)
      }
      return
    }
    enqueueLocked([value])
  }

  /// Hands events to the delivery queue. Called with `lock` held so that the order events
  /// reach the queue is the order the core produced them in.
  private func enqueueLocked(_ events: [JobEvent]) {
    guard !events.isEmpty else { return }
    delivery.async { [weak self] in
      guard let hub = self?.hub else { return }
      for event in events {
        hub.ingest(event)
      }
    }
  }
}

let jobEventThunk: pd_job_event_cb = { _, event, context in
  guard let context, let event else { return }
  Unmanaged<JobEventTrampoline>.fromOpaque(context).takeUnretainedValue().deliver(event.pointee)
}

/// Carries device events from whichever thread decoded a status frame onto the delivery
/// queue. Same lifetime rules as ``JobEventTrampoline``.
final class DeviceEventTrampoline: @unchecked Sendable {
  private let delivery: DispatchQueue
  private let lock = NSLock()
  private var continuations: [UUID: AsyncStream<DeviceEvent>.Continuation] = [:]

  init(delivery: DispatchQueue) {
    self.delivery = delivery
  }

  fileprivate func deliver(_ event: pd_device_event) {
    let value = DeviceEvent(bridging: event.rawValue)
    delivery.async { [weak self] in
      guard let self else { return }
      lock.lock()
      let targets = Array(continuations.values)
      lock.unlock()
      for continuation in targets {
        continuation.yield(value)
      }
    }
  }

  func makeStream() -> AsyncStream<DeviceEvent> {
    AsyncStream(bufferingPolicy: .unbounded) { continuation in
      let id = UUID()
      lock.lock()
      continuations[id] = continuation
      lock.unlock()
      continuation.onTermination = { [weak self] _ in
        guard let self else { return }
        lock.lock()
        continuations.removeValue(forKey: id)
        lock.unlock()
      }
    }
  }
}

let deviceEventThunk: pd_device_event_cb = { _, event, context in
  guard let context else { return }
  Unmanaged<DeviceEventTrampoline>.fromOpaque(context).takeUnretainedValue().deliver(event)
}

// MARK: - Job event hub

/// Fans one job's event stream out to any number of `AsyncStream`s and closures.
///
/// Every stored property is touched only from the delivery queue, so there is no lock in
/// here and no way for two observers to see two different orderings. Late subscribers get
/// the events recorded so far before the live ones, which mirrors `pd_subscribe_job`'s own
/// replay-then-stream behaviour.
final class JobEventHub: @unchecked Sendable {
  private let delivery: DispatchQueue
  private var recorded: [JobEvent] = []
  private var isFinished = false
  private var continuations: [UUID: AsyncStream<JobEvent>.Continuation] = [:]
  private var progressHandlers: [(JobEvent) -> Void] = []

  init(delivery: DispatchQueue) {
    self.delivery = delivery
  }

  /// Called on the delivery queue only.
  func ingest(_ event: JobEvent) {
    guard !isFinished else { return }
    recorded.append(event)
    for continuation in continuations.values {
      continuation.yield(event)
    }
    for handler in progressHandlers {
      handler(event)
    }
  }

  /// Called on the delivery queue only, after the terminal event has been ingested.
  ///
  /// The core emits a job's terminal event before it publishes the job's result
  /// (`PrinterRuntime::terminate` advances, then finishes), so the event reaches this
  /// queue strictly before the awaiter's settle block does. Closing the stream from the
  /// awaiter therefore cannot truncate it.
  func finish() {
    guard !isFinished else { return }
    isFinished = true
    for continuation in continuations.values {
      continuation.finish()
    }
    continuations.removeAll()
    progressHandlers.removeAll()
  }

  func makeStream() -> AsyncStream<JobEvent> {
    AsyncStream(bufferingPolicy: .unbounded) { continuation in
      let id = UUID()
      continuation.onTermination = { [weak self] _ in
        guard let self else { return }
        delivery.async { self.continuations.removeValue(forKey: id) }
      }
      delivery.async { [self] in
        for event in recorded {
          continuation.yield(event)
        }
        if isFinished {
          continuation.finish()
        } else {
          continuations[id] = continuation
        }
      }
    }
  }

  func addProgressHandler(_ handler: @escaping (JobEvent) -> Void) {
    delivery.async { [self] in
      for event in recorded {
        handler(event)
      }
      guard !isFinished else { return }
      progressHandlers.append(handler)
    }
  }
}

// MARK: - Result awaiter

/// Turns `pd_job_await`, which blocks, into something Swift can wait on.
///
/// The blocking call runs once per job on a global queue and its answer is cached, so
/// every `await job.result`, every completion closure and the end of every event stream
/// come from that single settlement. That is what makes the terminal notification fire
/// exactly once no matter how many observers a job has.
final class JobResultAwaiter: @unchecked Sendable {
  private let core: DriverCore
  private let handle: OpaquePointer
  private let authority: CompletionAuthority
  private let lock = NSLock()
  private var settled: JobResult?
  private var waiters: [(JobResult) -> Void] = []
  private var isRunning = false

  init(core: DriverCore, handle: OpaquePointer, authority: CompletionAuthority) {
    self.core = core
    self.handle = handle
    self.authority = authority
  }

  /// Registers `handler`, to be called exactly once on the delivery queue.
  func whenSettled(_ handler: @escaping (JobResult) -> Void) {
    lock.lock()
    if let settled {
      lock.unlock()
      core.delivery.async { handler(settled) }
      return
    }
    waiters.append(handler)
    let shouldStart = !isRunning
    isRunning = true
    lock.unlock()

    guard shouldStart else { return }
    // Not the delivery queue: this call blocks until the job is terminal, and blocking
    // the delivery queue would stall every event the job still has to report.
    DispatchQueue.global(qos: .userInitiated).async { [self] in
      var raw = pd_job_result()
      // timeout 0 waits indefinitely, and the core guarantees every job reaches a
      // terminal state — pd_destroy itself waits for that before returning.
      _ = pd_job_await(core.handle, handle, 0, &raw)
      let result = JobResult(raw, authority: authority)
      core.delivery.async { [self] in
        lock.lock()
        settled = result
        let pending = waiters
        waiters.removeAll()
        lock.unlock()
        for waiter in pending {
          waiter(result)
        }
      }
    }
  }
}
