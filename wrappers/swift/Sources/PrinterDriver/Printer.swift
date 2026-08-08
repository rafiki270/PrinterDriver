import CPrinterDriver
import Foundation

/// One configured device: transport, width and capability profile.
///
/// Owned by the ``PrinterDriver`` that created it and valid for that driver's lifetime,
/// which this object extends by holding it.
public final class Printer: @unchecked Sendable {
  let core: DriverCore
  /// Internal, not private, so the test target can reach the same `pd_printer*` the
  /// scripted-device support functions are keyed on.
  let handle: OpaquePointer
  private let lock = NSLock()
  private var deviceTrampoline: DeviceEventTrampoline?

  /// The stable printer id, either the one supplied at configuration or one derived from
  /// the endpoint.
  public let id: String

  /// The printable width the core will scale rasters to.
  public let widthDots: UInt32

  /// Which ordered fence this printer's profile answers.
  ///
  /// This is the ceiling on what a `done` result can ever rest on:
  /// ``CompletionMechanism/gsParenH`` can prove a fault-free cut,
  /// ``CompletionMechanism/gsR1`` can prove ordering, and
  /// ``CompletionMechanism/none`` can prove only that the transport took the bytes.
  public let completionMechanism: CompletionMechanism

  init(core: DriverCore, handle: OpaquePointer) {
    self.core = core
    self.handle = handle
    id = String(cString: pd_printer_id(handle))
    widthDots = pd_printer_width_dots(handle)
    completionMechanism = CompletionMechanism(bridging: pd_printer_completion(handle).rawValue)
  }

  // MARK: - Printing

  /// Submits a job.
  ///
  /// Returns as soon as the core has taken the payload; the printing itself is reported
  /// through the returned job's ``PrintJob/events`` and ``PrintJob/result``.
  ///
  /// - Important: Re-submitting a ``JobOptions/key`` that already has a job **does not
  ///   print**. It returns that job — the same object, in whatever state it is in. This
  ///   is the duplicate-ticket defence, and it survives an app restart. To print the same
  ///   key again on purpose, use ``forceReprint(key:options:)``.
  /// - Throws: ``PrinterDriverError`` when the payload is malformed.
  public func print(_ payload: Payload, options: JobOptions = JobOptions()) throws -> PrintJob {
    let handle = try payload.withABI { payloadPointer in
      try options.withABI { optionsPointer -> OpaquePointer in
        guard let job = pd_print(core.handle, self.handle, payloadPointer, optionsPointer) else {
          throw core.lastError()
        }
        return job
      }
    }
    return core.internJob(handle, authority: completionMechanism)
  }

  /// Submits a job and reports it through closures — the callback form of
  /// ``print(_:options:)`` for code that is not in an `async` context.
  ///
  /// - Parameters:
  ///   - onProgress: called for every ``JobEvent``, in order, including the ones already
  ///     recorded before this call.
  ///   - completion: called **exactly once**, with the tri-state terminal result. It fires
  ///     after the last `onProgress` call for that job, and it fires for a job that had
  ///     already finished before you asked.
  /// - Returns: the same handle ``print(_:options:)`` would return, so a caller can still
  ///   read state or attach an `AsyncStream` later.
  ///
  /// Both closures are delivered on the driver's serial delivery queue, not on the main
  /// queue and not on a core worker thread.
  @discardableResult
  public func print(
    _ payload: Payload,
    options: JobOptions = JobOptions(),
    onProgress: @escaping (JobEvent) -> Void = { _ in },
    completion: @escaping (JobResult) -> Void
  ) throws -> PrintJob {
    let job = try print(payload, options: options)
    job.observe(onProgress: onProgress, completion: completion)
    return job
  }

  /// Prints a deliberate duplicate of an already-submitted key.
  ///
  /// Reuses the original payload and prepends the reprint banner and attempt counter, so
  /// the paper says what it is. This is the only way to print a key twice.
  ///
  /// - Throws: ``PrinterDriverError`` when the key is unknown, or when its job was
  ///   reconstructed from the journal — those records carry what happened to a job, never
  ///   what it contained.
  public func forceReprint(key: String, options: JobOptions = JobOptions()) throws -> PrintJob {
    let handle = try key.withCString { keyPointer in
      try options.withABI { optionsPointer -> OpaquePointer in
        guard
          let job = pd_force_reprint(core.handle, self.handle, keyPointer, optionsPointer)
        else {
          throw core.lastError()
        }
        return job
      }
    }
    return core.internJob(handle, authority: completionMechanism)
  }

  // MARK: - Device state

  /// The last known device state. A snapshot, never a live query, so it cannot block
  /// behind a print.
  public func status() -> DeviceStatus {
    DeviceStatus(pd_printer_status(core.handle, handle))
  }

  /// Queues a status round trip behind any active job and waits for the answer.
  ///
  /// - Parameter timeoutMilliseconds: 0 means the ABI default of 2000.
  /// - Important: Blocks the calling thread. Do not call it from the main thread or from
  ///   inside an event handler.
  public func refreshStatus(timeoutMilliseconds: UInt32 = 0) -> DeviceStatus {
    DeviceStatus(pd_printer_refresh_status(core.handle, handle, timeoutMilliseconds))
  }

  /// The per-printer event stream that replaces availability ping-polling.
  ///
  /// Each access returns a fresh stream carrying events from that point on; there is no
  /// replay, because a device event describes a moment rather than a state. Read
  /// ``status()`` for where things stand now.
  public var deviceEvents: AsyncStream<DeviceEvent> {
    lock.lock()
    let trampoline: DeviceEventTrampoline
    if let existing = deviceTrampoline {
      trampoline = existing
      lock.unlock()
    } else {
      trampoline = DeviceEventTrampoline(delivery: core.delivery)
      deviceTrampoline = trampoline
      lock.unlock()
      // Once only: pd.h has no unsubscribe, so a second subscription would double every
      // event for the life of the driver.
      core.retain(trampoline: trampoline)
      pd_subscribe_device(
        core.handle, handle, deviceEventThunk,
        Unmanaged.passUnretained(trampoline).toOpaque())
    }
    return trampoline.makeStream()
  }

  /// Pulses the cash drawer.
  public func openCashDrawer() {
    pd_open_cash_drawer(core.handle, handle)
  }

  /// Waits until this printer's queue is empty and its active job is terminal.
  ///
  /// - Important: Blocks the calling thread.
  public func drain() {
    pd_printer_drain(core.handle, handle)
  }
}
