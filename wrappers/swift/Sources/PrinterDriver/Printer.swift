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

  /// What ``completionMechanism`` is actually worth — docs/compatibility-brief.md §28.
  ///
  /// Not a confidence level, and it changes nothing about what a job reports. It answers
  /// a different question, and answers it *before* anything has been printed: should this
  /// printer's fence be trusted on the strength of a profile alone?
  ///
  /// ``Provenance/documented`` means the manufacturer's own command documentation lists
  /// the mechanism for this model — in the shipped database, Epson and nobody else.
  /// ``Provenance/probed`` means this driver asked the installed hardware over the
  /// installed interface path and it answered, which beats any datasheet and is specific
  /// to that path. ``Provenance/unverified`` means neither, which is what "ESC/POS
  /// compatible" on a spec sheet amounts to — and is the signal that running `pdctl probe`
  /// against the site's hardware is worth the trip.
  public let completionProvenance: Provenance

  /// The command language this printer's profile is driven in.
  ///
  /// Anything but ``CommandLanguage/escPos`` is refused with ``FailureReason/unsupported``
  /// before a byte is written: a Zebra or a Brother is not an ESC/POS printer, and sending
  /// it ESC/POS produces a metre of text rather than a label.
  public let language: CommandLanguage

  init(core: DriverCore, handle: OpaquePointer) {
    self.core = core
    self.handle = handle
    id = String(cString: pd_printer_id(handle))
    widthDots = pd_printer_width_dots(handle)
    completionMechanism = CompletionMechanism(bridging: pd_printer_completion(handle).rawValue)
    completionProvenance = Provenance(
      bridging: pd_printer_completion_provenance(handle).rawValue)
    language = CommandLanguage(bridging: pd_printer_language(handle).rawValue)
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
    return core.internJob(handle, mechanism: completionMechanism)
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
  /// - Parameter options: pass ``ReprintOptions/banner`` `false` only for a receipt
  ///   where the banner is inappropriate; the attempt counter still increments either
  ///   way, so the journal records the duplicate whatever the paper says.
  /// - Throws: ``PrinterDriverError`` when the key is unknown, or when its job was
  ///   reconstructed from the journal — those records carry what happened to a job, never
  ///   what it contained.
  public func forceReprint(key: String, options: ReprintOptions = ReprintOptions())
    throws -> PrintJob
  {
    var effective = options
    effective.job.key = key
    let handle = try key.withCString { keyPointer in
      try effective.withABI { optionsPointer -> OpaquePointer in
        guard
          let job = pd_force_reprint_opts(
            core.handle, self.handle, keyPointer, optionsPointer)
        else {
          throw core.lastError()
        }
        return job
      }
    }
    return core.internJob(handle, mechanism: completionMechanism)
  }

  /// Prints a deliberate duplicate with the plain submission options, banner on.
  public func forceReprint(key: String, options: JobOptions) throws -> PrintJob {
    try forceReprint(key: key, options: ReprintOptions(options))
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

  /// Pulses the cash drawer and tells the caller nothing.
  ///
  /// Kept because it is ABI, and because a caller that genuinely does not want to wait is
  /// entitled to say so. Everything new should use ``openDrawer(channel:pulseMilliseconds:)``,
  /// which reports what actually happened.
  public func openCashDrawer() {
    pd_open_cash_drawer(core.handle, handle)
  }

  // MARK: - M14: cash drawer (docs/cash-drawer.md)

  /// What this printer's drawer port is, and what is known about it.
  public var drawerCapabilities: DrawerCapabilities {
    DrawerCapabilities(pd_printer_drawer_capabilities(handle))
  }

  /// Fires the drawer and reports what was observed, never a boolean.
  ///
  /// The sequence: read the switch first (a drawer that is already out is not pulsed
  /// again), take the printer's peripheral lane so the pulse cannot land inside a fenced
  /// job, emit one queued pulse, watch the switch for the profile's verification window,
  /// and hold the manufacturer cooldown before the next one.
  ///
  /// Refused — zero bytes written, ``DrawerState/unknown`` — when this model has no drawer
  /// port, when its kick method is one this engine does not drive, or when the electrical
  /// standard is ``DrawerPortStandard/unknown``. RJ11/RJ12-looking drawer connectors are
  /// not a universal electrical standard, and an unclassified port is never energised.
  ///
  /// - Parameters:
  ///   - channel: 1 for drive 1 (Epson pin 2), 2 for drive 2 (pin 5). Clamped to what the
  ///     profile documents.
  ///   - pulseMilliseconds: 0 uses the profile's own default, 200 ms on the documented
  ///     24 V families.
  /// - Important: Blocks the calling thread until the sequence reaches a verdict.
  @discardableResult
  public func openDrawer(channel: UInt8 = 1, pulseMilliseconds: UInt16 = 0) -> DrawerResult {
    var request = pd_drawer_request(channel: channel, pulse_ms: pulseMilliseconds)
    return DrawerResult(pd_drawer_open(core.handle, handle, &request))
  }

  /// Reads the drawer switch without firing anything.
  ///
  /// Safe on hardware ``openDrawer(channel:pulseMilliseconds:)`` refuses, which is what
  /// makes it the first half of the calibration procedure.
  ///
  /// - Important: Blocks the calling thread.
  public func readDrawerSensor(timeoutMilliseconds: UInt32 = 1500) -> DrawerReading {
    DrawerReading(pd_drawer_read_sensor(core.handle, handle, timeoutMilliseconds))
  }

  /// Records which sense level means "open" for the drawer attached to this printer, and
  /// persists it in the driver's storage directory.
  ///
  /// The procedure is an operator's: ask for the drawer to be closed, read the sensor, ask
  /// for it to be opened, read again, and pass the level observed while it was open.
  ///
  /// - Returns: `true` when it was persisted; `false` when it applies to this process only
  ///   (an in-memory driver, or a storage directory that cannot be written).
  @discardableResult
  public func calibrateDrawerPolarity(highMeansOpen: Bool) -> Bool {
    pd_drawer_calibrate_polarity(core.handle, handle, highMeansOpen ? 1 : 0) == 1
  }

  /// Whether a polarity has been measured for this printer. Until it has, a sensor reading
  /// carries a level and no interpretation.
  public var isDrawerPolarityCalibrated: Bool {
    pd_drawer_polarity_calibrated(core.handle, handle) == 1
  }

  /// Meaningful only while ``isDrawerPolarityCalibrated`` is true.
  public var drawerHighMeansOpen: Bool {
    pd_drawer_high_means_open(core.handle, handle) == 1
  }

  /// Waits until this printer's queue is empty and its active job is terminal.
  ///
  /// - Important: Blocks the calling thread.
  public func drain() {
    pd_printer_drain(core.handle, handle)
  }
}
