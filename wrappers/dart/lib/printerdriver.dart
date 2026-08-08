/// Dart bindings for the PrinterDriver SDK.
///
/// Three nouns, as in every other wrapper (docs/api.md §2): a [PrinterDriver] owns
/// printers, a [Printer] accepts jobs, a [PrintJob] reports what happened to one. The
/// whole implementation — ESC/POS encoding, completion fences, retry policy, the job
/// state machine — is in the C++ core behind `pd.h`; this layer converts types and
/// adapts callbacks to futures and streams, and decides nothing.
///
/// ```dart
/// final driver = PrinterDriver.open(storageDirectory: '/var/lib/pos/printerdriver');
/// final kitchen = driver.addTcpPrinter(host: '192.168.1.101', widthDots: 576);
///
/// final result = await kitchen.send(
///   Payload.document([DocumentOp.line('TABLE 12'), DocumentOp.feed(2)]),
///   key: 'order-7F3A-92C1#kitchen-1',
///   onProgress: (event) => ticketUi.update(event.state),
/// );
///
/// switch (result) {
///   case JobDone(:final confidence): markPrinted(confidence);
///   case JobFailed(:final reason):   showFailure(reason);
///   case JobUnknown():               askOperator();
/// }
/// ```
library;

import 'dart:async';
import 'dart:ffi';

import 'src/allocation.dart';
import 'src/bindings.dart';
import 'src/enums.dart';
import 'src/library_loader.dart';
import 'src/types.dart';

export 'src/enums.dart';
export 'src/library_loader.dart'
    show
        PrinterDriverLibraryNotFound,
        defaultLibraryFileName,
        loadPrinterDriverLibrary,
        printerDriverLibPathVariable;
export 'src/types.dart';

/// An operation the native library refused, carrying the driver's own explanation.
final class PrinterDriverException implements Exception {
  PrinterDriverException(this.message);

  /// `pd_last_error` for the call that failed.
  final String message;

  @override
  String toString() => 'PrinterDriverException: $message';
}

/// The SDK entry point: owns the native driver, its printers and its jobs.
///
/// One instance per application. Everything it hands out — [Printer], [PrintJob] — is
/// owned by it and dies with [dispose].
final class PrinterDriver {
  PrinterDriver._(this._bindings, this._handle, this._logCallback);

  /// Opens the native library and creates a driver.
  ///
  /// [storageDirectory] is where the job journal lives; null or empty means in-memory,
  /// which means no crash recovery — a POS integration should always pass a path.
  /// [fsyncDisabled] is for tests only: it trades the durability rule for speed.
  ///
  /// [onLog] receives ABI-level diagnostics. It is invoked synchronously, on the thread
  /// that called into the ABI, and must not call back into this driver.
  ///
  /// [libraryPath] overrides library resolution; see [loadPrinterDriverLibrary] for the
  /// search order otherwise. [library] binds an already-open library, which is how an
  /// application whose native code is linked into the executable
  /// (`DynamicLibrary.process()`) uses this package.
  factory PrinterDriver.open({
    String? storageDirectory,
    bool fsyncDisabled = false,
    void Function(String message)? onLog,
    String? libraryPath,
    DynamicLibrary? library,
  }) {
    final bindings = PrinterDriverBindings(
        library ?? loadPrinterDriverLibrary(path: libraryPath));

    // isolateLocal, not listener: the message is a temporary the ABI frees when the
    // call returns, so it has to be read inside the call. That is sound because a log
    // hook only ever fires on the thread that called into the ABI, unlike the job and
    // device callbacks below.
    NativeCallable<PdLogCbNative>? logCallback;
    if (onLog != null) {
      logCallback = NativeCallable<PdLogCbNative>.isolateLocal(
        (Pointer<Char> message, Pointer<Void> ctx) =>
            onLog(readNativeString(message)),
      );
    }

    final handle = Arena.using((arena) {
      final config = arena.allocate<PdConfig>(sizeOf<PdConfig>());
      config.ref.storageDirectory = arena.string(storageDirectory);
      config.ref.fsyncDisabled = fsyncDisabled ? 1 : 0;
      config.ref.log = logCallback?.nativeFunction ?? nullptr;
      return bindings.create(config);
    });

    if (handle == nullptr) {
      logCallback?.close();
      // pd_create is the one call with no handle to hang an error on: only a storage
      // directory that cannot be created can cause it.
      throw PrinterDriverException(
        'pd_create failed; the storage directory could not be created'
        '${storageDirectory == null ? '' : ' ($storageDirectory)'}',
      );
    }

    return PrinterDriver._(bindings, handle, logCallback);
  }

  final PrinterDriverBindings _bindings;
  final Pointer<PdDriver> _handle;
  final NativeCallable<PdLogCbNative>? _logCallback;

  /// Interned by handle address: pd.h guarantees the same underlying object always maps
  /// to the same pointer, so idempotency-key dedupe is visible here as Dart identity.
  final Map<int, PrintJob> _jobs = <int, PrintJob>{};
  final Map<int, Printer> _printers = <int, Printer>{};

  bool _disposed = false;

  /// Whether [dispose] has run. Every other member throws once it has.
  bool get isDisposed => _disposed;

  /// Why the last call on this driver returned null; empty when it succeeded.
  String get lastError => readNativeString(_bindings.lastError(_handle));

  /// The profile ids [addTcpPrinter] accepts, enumerated from the library rather than
  /// hardcoded.
  List<String> get profileIds {
    _checkAlive();
    final ids = <String>[];
    final array = _bindings.profileIds();
    for (var i = 0; array[i] != nullptr; i++) {
      ids.add(readNativeString(array[i]));
    }
    return ids;
  }

  /// Adds a network printer. Nothing is connected yet: the transport dials when the
  /// first job needs it and the device event stream reports what happens.
  ///
  /// Zero means the documented default for every numeric argument: port 9100, 576 dots,
  /// a 3 s connect timeout. [profileId] null or empty means `generic`.
  Printer addTcpPrinter({
    required String host,
    int port = 0,
    String? printerId,
    int widthDots = 0,
    String? profileId,
    Duration? connectTimeout,
  }) {
    _checkAlive();
    final handle = Arena.using((arena) {
      final config = arena.allocate<PdTcpConfig>(sizeOf<PdTcpConfig>());
      config.ref
        ..printerId = arena.string(printerId)
        ..host = arena.string(host)
        ..port = port
        ..widthDots = widthDots
        ..profileId = arena.string(profileId)
        ..connectTimeoutMs = connectTimeout?.inMilliseconds ?? 0;
      return _bindings.addPrinterTcp(_handle, config);
    });
    if (handle == nullptr) {
      throw PrinterDriverException(lastError);
    }
    return _internPrinter(handle);
  }

  /// Any job this driver knows about, including one reloaded from the journal after a
  /// restart, or null when the key is unknown.
  ///
  /// This is how a crash is resolved: a job that reached `SendStarted` without an
  /// acknowledgement comes back as [JobUnknown], never auto-retried.
  PrintJob? findJob(String key) {
    _checkAlive();
    final handle = Arena.using(
      (arena) => _bindings.findJob(_handle, arena.string(key)),
    );
    return handle == nullptr ? null : _internJob(handle);
  }

  /// Paper to job: resolves the four-character `V:` code printed on a receipt
  /// (docs/api.md §14).
  ///
  /// Accepts either of a job's identifiers, the print fence's or the cut fence's, and
  /// answers most-recent-first — the sequence wraps, and the receipt somebody is
  /// holding is far more likely to be the recent one. Includes jobs reconstructed from
  /// the journal, so a receipt printed before the last restart still resolves. Null
  /// when no job on this driver ever carried that token.
  PrintJob? jobByToken(String token) {
    _checkAlive();
    final handle = Arena.using(
      (arena) => _bindings.jobByToken(_handle, arena.string(token)),
    );
    return handle == nullptr ? null : _internJob(handle);
  }

  /// The two characters every identifier this driver issues starts with: which driver
  /// instance owns an echo, and therefore which instance printed a given receipt.
  ///
  /// Persisted in the storage directory, so it survives a restart. A token that does
  /// not start with this came from somewhere else — the case
  /// [DeviceEvent.foreignWriterDetected] reports.
  String get instanceNonce {
    _checkAlive();
    return readNativeString(_bindings.instanceNonce(_handle));
  }

  /// Test support: attaches a printer whose transport is an in-process scripted device.
  ///
  /// Only works against a library built from the `printerdriver_capi_testing` target;
  /// the library an application ships does not export `pd_add_printer_scripted` at all,
  /// and this throws there rather than pretending. It exists so that a POS integration
  /// can test its own printing paths — including the paths that must handle
  /// [JobUnknown] — without hardware.
  ///
  /// [script] selects the device's behaviour: `ok` (healthy `GS ( H` printer, jobs
  /// reach Done at [ConfidenceLevel.cutFaultFree]), `gsr1` (queued `GS r 1` fence only,
  /// so jobs cap at [ConfidenceLevel.cutProcessed]), `silent` (accepts bytes, never
  /// answers the completion marker, so jobs end [JobUnknown]), `paperout` (strict
  /// preflight refuses before any payload byte) and `refuse` (the connection fails).
  Printer addScriptedPrinterForTesting({
    required String script,
    String? printerId,
  }) {
    _checkAlive();
    if (!_bindings.hasTestSupport) {
      throw PrinterDriverException(
        'this native library exports no pd_add_printer_scripted: it was built from '
        'printerdriver_capi_shared, which by design carries no test doubles. Build '
        'the printerdriver_capi_testing target and point PRINTERDRIVER_LIB_PATH at it.',
      );
    }
    final handle = Arena.using(
      (arena) => _bindings.addPrinterScripted(
        _handle,
        arena.string(printerId),
        arena.string(script),
      ),
    );
    if (handle == nullptr) {
      throw PrinterDriverException('unknown scripted device id: $script');
    }
    return _internPrinter(handle);
  }

  /// Stops every printer worker, waits for in-flight jobs to reach a terminal state and
  /// frees every handle this driver ever returned.
  ///
  /// Blocks for as long as the in-flight jobs need, so await the jobs you care about
  /// first: any [PrintJob.result] still pending when this runs completes with a
  /// [StateError] instead of a printing outcome, because the answer is no longer
  /// obtainable.
  void dispose() {
    if (_disposed) return;
    // Before pd_destroy, so that a callback message already queued for this isolate
    // finds a driver that says it is gone rather than reading through freed handles.
    _disposed = true;
    _bindings.destroy(_handle);
    for (final job in _jobs.values) {
      job._onDriverDisposed();
    }
    for (final printer in _printers.values) {
      printer._onDriverDisposed();
    }
    _jobs.clear();
    _printers.clear();
    _logCallback?.close();
  }

  Printer _internPrinter(Pointer<PdPrinter> handle) =>
      _printers[handle.address] ??= Printer._(this, handle);

  PrintJob _internJob(Pointer<PdJob> handle) =>
      _jobs[handle.address] ??= PrintJob._(this, handle);

  void _checkAlive() {
    if (_disposed) {
      throw StateError('this PrinterDriver has been disposed');
    }
  }
}

/// One printer, and the only thing that accepts a job.
final class Printer {
  Printer._(this._driver, this._handle)
      : id = readNativeString(_driver._bindings.printerId(_handle));

  final PrinterDriver _driver;
  final Pointer<PdPrinter> _handle;

  /// Stable for the life of the driver.
  final String id;

  PrinterDriverBindings get _bindings => _driver._bindings;

  /// The printable width in dots: 384, 504 or 576 on the deployed hardware.
  int get widthDots {
    _driver._checkAlive();
    return _bindings.printerWidthDots(_handle);
  }

  /// Which ordered fence this printer's capability profile answers, and therefore the
  /// ceiling on what any job on it can claim.
  CompletionMechanism get completion {
    _driver._checkAlive();
    return CompletionMechanism.fromNative(_bindings.printerCompletion(_handle));
  }

  /// The last known device state. A snapshot, never a live query, so it cannot block
  /// behind a print — and it says [DeviceStatus.observed] is false until the device has
  /// actually answered something.
  DeviceStatus get status {
    _driver._checkAlive();
    return DeviceStatus.fromNative(
        _bindings.printerStatus(_driver._handle, _handle));
  }

  /// Queues a status round trip behind any active job and waits for the answer.
  ///
  /// Blocks this isolate for up to [timeout] (2 s when null). [status] is what a UI
  /// should read; this is for a diagnostics screen that asked.
  DeviceStatus refreshStatus({Duration? timeout}) {
    _driver._checkAlive();
    return DeviceStatus.fromNative(
      _bindings.printerRefreshStatus(
        _driver._handle,
        _handle,
        timeout?.inMilliseconds ?? 0,
      ),
    );
  }

  void openCashDrawer() {
    _driver._checkAlive();
    _bindings.openCashDrawer(_driver._handle, _handle);
  }

  /// Blocks until this printer's queue is empty and its active job is terminal.
  void drain() {
    _driver._checkAlive();
    _bindings.printerDrain(_driver._handle, _handle);
  }

  /// The device event stream that replaces availability ping-polling: `online/offline`,
  /// `coverOpen/Closed`, `paperOut/NearEnd/Ok`, `cutterError`,
  /// `recoverable/unrecoverableError`, `connectionLost/Restored`.
  ///
  /// Live, and a broadcast stream: events are delivered as the core decodes them.
  /// Subscribing costs nothing on the printer — the core is already reading the status
  /// backchannel — and there is no unsubscribe in the ABI, so the first listener wires
  /// it up for the life of the driver.
  Stream<DeviceEvent> get events {
    _driver._checkAlive();
    final controller =
        _deviceEvents ??= StreamController<DeviceEvent>.broadcast();
    if (_deviceCallback == null) {
      // A listener callback, invoked from whichever core thread decoded the status. It
      // is safe here where it would not be for job events: a device event is an int
      // passed by value, so nothing has to outlive the native call.
      final callback = NativeCallable<PdDeviceEventCbNative>.listener(
        (Pointer<PdPrinter> printer, int event, Pointer<Void> ctx) {
          if (_driver.isDisposed || controller.isClosed) return;
          controller.add(DeviceEvent.fromNative(event));
        },
      );
      callback.keepIsolateAlive = false;
      _deviceCallback = callback;
      _bindings.subscribeDevice(
        _driver._handle,
        _handle,
        callback.nativeFunction,
        nullptr,
      );
    }
    return controller.stream;
  }

  StreamController<DeviceEvent>? _deviceEvents;
  NativeCallable<PdDeviceEventCbNative>? _deviceCallback;

  /// Submits a job and returns immediately.
  ///
  /// Resubmitting a key that already has a job does not print: it returns that job,
  /// whatever state it is in — the same Dart object, because the ABI returns the same
  /// handle. Printing the same key again is only possible through [forceReprint].
  PrintJob print(Payload payload, {JobOptions options = const JobOptions()}) {
    _driver._checkAlive();
    final handle = Arena.using((arena) {
      final nativePayload = arena.allocate<PdPayload>(sizeOf<PdPayload>());
      payload.fillNative(arena, nativePayload);
      final nativeOptions =
          arena.allocate<PdJobOptions>(sizeOf<PdJobOptions>());
      options.fillNative(arena, nativeOptions);
      return _bindings.print(
          _driver._handle, _handle, nativePayload, nativeOptions);
    });
    if (handle == nullptr) {
      throw PrinterDriverException(_driver.lastError);
    }
    return _driver._internJob(handle);
  }

  /// The closure form of [print] (docs/api.md §12).
  ///
  /// [onProgress] receives every [JobEvent] of the job, in order; the returned future —
  /// and [onResult], when given — carries the terminal answer exactly once.
  ///
  /// ```dart
  /// final result = await kitchen.send(receipt,
  ///     key: 'order-7F3A-92C1#kitchen-1',
  ///     onProgress: (event) => ticketUi.update(event.state));
  /// ```
  Future<JobResult> send(
    Payload payload, {
    String? key,
    JobOptions options = const JobOptions(),
    void Function(JobEvent event)? onProgress,
    void Function(JobResult result)? onResult,
  }) async {
    final job = print(
      payload,
      options: key == null
          ? options
          : JobOptions(
              key: key,
              cut: options.cut,
              openDrawer: options.openDrawer,
              preflight: options.preflight,
              timeout: options.timeout,
            ),
    );
    if (onProgress != null) {
      await for (final event in job.events) {
        onProgress(event);
      }
    }
    final result = await job.result;
    onResult?.call(result);
    return result;
  }

  /// A deliberate duplicate of an already-submitted key.
  ///
  /// Reuses the original payload and prepends the reprint banner and attempt counter.
  /// Throws when the key is unknown, or when its job was reconstructed from the journal
  /// — those records carry what happened to a job, never what it contained.
  ///
  /// Pass `ReprintOptions(banner: false)` only for a receipt where the banner is
  /// inappropriate; the attempt counter still increments either way, so the journal
  /// records the duplicate whatever the paper says.
  PrintJob forceReprint(String key,
      {ReprintOptions options = const ReprintOptions()}) {
    _driver._checkAlive();
    final handle = Arena.using((arena) {
      final nativeOptions =
          arena.allocate<PdReprintOptions>(sizeOf<PdReprintOptions>());
      options.fillNative(arena, nativeOptions);
      return _bindings.forceReprintOpts(
        _driver._handle,
        _handle,
        arena.string(key),
        nativeOptions,
      );
    });
    if (handle == nullptr) {
      throw PrinterDriverException(_driver.lastError);
    }
    return _driver._internJob(handle);
  }

  /// Test support: what the scripted device behind this printer actually received.
  ///
  /// Null unless this printer came from
  /// [PrinterDriver.addScriptedPrinterForTesting].
  ScriptedDevice? get scriptedDeviceForTesting =>
      _bindings.hasTestSupport ? ScriptedDevice._(this) : null;

  void _onDriverDisposed() {
    _deviceCallback?.close();
    _deviceCallback = null;
    _deviceEvents?.close();
  }

  @override
  String toString() => 'Printer($id)';
}

/// Test support: the in-process device behind a scripted printer.
///
/// Backed by `capi/tests/pd_test_support.h`, which exists only in the library built
/// from the `printerdriver_capi_testing` target.
final class ScriptedDevice {
  ScriptedDevice._(this._printer);

  final Printer _printer;

  /// How many payload bytes the device has been handed, excluding framing.
  int get printDataBytes {
    _printer._driver._checkAlive();
    return _printer._bindings.testPrintDataBytes(_printer._handle);
  }

  /// How many cuts the device has performed.
  int get cuts {
    _printer._driver._checkAlive();
    return _printer._bindings.testCuts(_printer._handle);
  }

  /// Whether [needle] appears anywhere in what the device received.
  bool received(String needle) {
    _printer._driver._checkAlive();
    return Arena.using(
          (arena) => _printer._bindings
              .testReceivedContains(_printer._handle, arena.string(needle)),
        ) !=
        0;
  }
}

/// One submitted job, and the answer to "did it print?".
///
/// ## How the events reach Dart
///
/// pd.h hands a job event to its callback **by value**, so the copy outlives the native
/// call that produced it. That is what makes a `NativeCallable.listener` — the only
/// kind of callback a foreign thread may invoke, and one that runs its Dart function
/// after the native call has already returned — able to read the event at all.
///
/// So [events] is live: every transition reaches Dart as the core records it, not as a
/// batch once the job settles. A late subscriber still gets the whole story, because
/// the events recorded so far are replayed to it before the live ones, and the stream
/// closes after the terminal event.
///
/// Ordering is the core's guarantee, not this wrapper's: `pd_subscribe_job` replays the
/// recorded history and then streams, and the last event a job emits is always a
/// terminal one.
final class PrintJob {
  PrintJob._(this._driver, this._handle)
      : id = readNativeString(_driver._bindings.jobId(_handle)),
        key = readNativeString(_driver._bindings.jobKey(_handle)) {
    // Keeps the isolate alive on purpose, and only until the job settles: a program
    // whose last outstanding work is `await job.result` must not exit before the
    // printer has answered. _settle() closes it.
    _tick = NativeCallable<PdJobEventCbNative>.listener(_onNativeEvent);
    _bindings.subscribeJob(
        _driver._handle, _handle, _tick.nativeFunction, nullptr);
    // A job restored from the journal can be terminal already and emit nothing further,
    // in which case no tick is ever coming.
    _pump();
  }

  /// The receipt verification identifier this attempt printed under — the four
  /// characters the ticket carries as `V:` (docs/api.md §14).
  ///
  /// Null until the job reaches a worker, and null for good on a printer whose fence is
  /// not `GS ( H`: the identifier *is* the wire token, so a printer with no wire token
  /// has none to print. Resolve one with [PrinterDriver.jobByToken].
  String? get printToken {
    _driver._checkAlive();
    final value = readNativeString(_bindings.jobPrintToken(_handle));
    return value.isEmpty ? null : value;
  }

  /// The identifier the job's cut fence carried. Same rules as [printToken]; it
  /// resolves through [PrinterDriver.jobByToken] too.
  String? get cutToken {
    _driver._checkAlive();
    final value = readNativeString(_bindings.jobCutToken(_handle));
    return value.isEmpty ? null : value;
  }

  final PrinterDriver _driver;
  final Pointer<PdJob> _handle;

  /// The SDK's own id for this attempt. Stable for the life of the driver.
  final String id;

  /// The idempotency key, generated when the caller did not supply one.
  final String key;

  late final NativeCallable<PdJobEventCbNative> _tick;
  // Every event seen so far, so a subscriber that attaches late is not told a shorter
  // story than one that was there from the start.
  final List<JobEvent> _recorded = <JobEvent>[];
  final List<StreamController<JobEvent>> _listeners =
      <StreamController<JobEvent>>[];
  final Completer<JobResult> _result = Completer<JobResult>();
  Timer? _recheck;
  int _recheckAttempt = 0;
  bool _settled = false;
  Object? _abandoned;

  // Belt and braces. A job settles on its terminal *event*, because pd.h guarantees the
  // last event a job emits is a terminal one and closing the stream on anything earlier
  // would truncate it. These re-checks exist only for the case that contract is somehow
  // not met — a job the core reports terminal with no terminal event behind it — so
  // that a caller waits milliseconds rather than forever.
  static const List<int> _recheckDelaysMs = <int>[1, 2, 4, 8, 16, 32];

  PrinterDriverBindings get _bindings => _driver._bindings;

  /// Which attempt this is: 1 for an original, higher for a [Printer.forceReprint].
  int get attempt {
    _driver._checkAlive();
    return _bindings.jobAttempt(_handle);
  }

  /// A live read of where the job is now. A snapshot, not an event: for a progress
  /// indicator, not for a record of what happened.
  JobState get currentState {
    _driver._checkAlive();
    return JobState.fromNative(_bindings.jobCurrentState(_handle));
  }

  /// A live read of what the current claim rests on. See [currentState].
  ConfidenceLevel get currentConfidence {
    _driver._checkAlive();
    return ConfidenceLevel.fromNative(_bindings.jobConfidence(_handle));
  }

  /// Whether the job has stopped moving.
  bool get isTerminal {
    _driver._checkAlive();
    return _bindings.jobIsTerminal(_handle) != 0;
  }

  /// Every event the job records, in order, ending with a terminal one.
  ///
  /// Live: each transition arrives as the core records it. Can be listened to more than
  /// once and after the job has finished — a new subscription replays what has happened
  /// so far and then follows the rest, so attaching late loses nothing. The stream
  /// closes after the terminal event; it is a completion signal, not a result, so read
  /// [result] for the tri-state answer.
  Stream<JobEvent> get events {
    late final StreamController<JobEvent> controller;
    controller = StreamController<JobEvent>(
      onListen: () {
        for (final event in _recorded) {
          controller.add(event);
        }
        final abandoned = _abandoned;
        if (abandoned != null) {
          controller.addError(abandoned);
          controller.close();
        } else if (_settled) {
          controller.close();
        } else {
          _listeners.add(controller);
        }
      },
      onCancel: () {
        _listeners.remove(controller);
      },
    );
    return controller.stream;
  }

  /// The terminal answer: [JobDone], [JobFailed] or [JobUnknown]. Never a boolean.
  ///
  /// Completes exactly once, including for a job that was already finished when this
  /// handle was obtained through [PrinterDriver.findJob] after a restart.
  Future<JobResult> get result => _result.future;

  /// One transition, by value, from whichever core thread produced it.
  void _onNativeEvent(Pointer<PdJob> job, PdJobEvent event, Pointer<Void> ctx) {
    if (_settled || _abandoned != null) return;
    final value = JobEvent.fromNative(event);
    _recorded.add(value);
    // A copy: a handler may cancel its subscription, which mutates _listeners.
    for (final listener in List<StreamController<JobEvent>>.of(_listeners)) {
      listener.add(value);
    }
    _recheckAttempt = 0;
    _pump();
  }

  /// Mirrors the core's own definition (`JobRecord::isTerminal`). `heldOffline` is a
  /// queue-addon state a job comes back out of, so it is deliberately not in here.
  static bool _isTerminalState(JobState state) =>
      state == JobState.doneSoftware ||
      state == JobState.physicallyVerified ||
      state == JobState.failedKnown ||
      state == JobState.unknown;

  void _pump() {
    if (_settled || _driver.isDisposed) return;
    if (_recorded.isNotEmpty && _isTerminalState(_recorded.last.state)) {
      _settle();
      return;
    }
    if (_recheckAttempt < _recheckDelaysMs.length) {
      _recheck?.cancel();
      _recheck = Timer(
        Duration(milliseconds: _recheckDelaysMs[_recheckAttempt++]),
        _pump,
      );
      return;
    }
    // The re-check budget is spent and no terminal event arrived. Settling on the
    // core's own answer is better than hanging; the result is still read from the ABI,
    // so nothing here invents an outcome.
    if (_bindings.jobIsTerminal(_handle) != 0) {
      _settle();
    }
  }

  void _settle() {
    _settled = true;
    _recheck?.cancel();
    _recheck = null;

    final result = _readResult();

    // The job is terminal, so pd.h's "the last event a job ever emits is a terminal
    // one" means nothing will call this again.
    _tick.close();

    for (final listener in List<StreamController<JobEvent>>.of(_listeners)) {
      listener.close();
    }
    _listeners.clear();
    _result.complete(result);
  }

  JobResult _readResult() => Arena.using((arena) {
        final out = arena.allocate<PdJobResult>(sizeOf<PdJobResult>());
        // 1 ms, not 0: the job is terminal so this returns at once, and 0 is pd.h's
        // "wait forever", which is not a licence worth handing to a UI isolate.
        if (_bindings.jobAwait(_driver._handle, _handle, 1, out) != 1 &&
            _bindings.jobAwait(_driver._handle, _handle, 1000, out) != 1) {
          throw PrinterDriverException(
            'pd_job_await did not produce a result for a job pd_job_is_terminal '
            'reported as terminal (job $id, key $key)',
          );
        }
        return jobResultFromNative(out.ref);
      });

  void _onDriverDisposed() {
    _recheck?.cancel();
    _recheck = null;
    _tick.close();
    if (_settled) return;
    _settled = true;
    final error = StateError(
      'the PrinterDriver was disposed before job $id (key $key) reached a terminal '
      'state, so its outcome can no longer be read',
    );
    _abandoned = error;
    // ignore() after each: a caller that disposed the driver without awaiting its jobs
    // asked for exactly this, and it must not also arrive as an unhandled async error.
    for (final listener in List<StreamController<JobEvent>>.of(_listeners)) {
      listener.addError(error);
      listener.close();
    }
    _listeners.clear();
    if (!_result.isCompleted) {
      _result.completeError(error);
      _result.future.ignore();
    }
  }

  @override
  String toString() => 'PrintJob($id, key: $key)';
}
