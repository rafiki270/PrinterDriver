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
import 'src/custom_methods.dart';
import 'src/enums.dart';
import 'src/library_loader.dart';
import 'src/transport.dart';
import 'src/types.dart';

// The vtable signatures only. They are part of the custom-transport API — a caller
// naming its own `connect` symbol has to spell the type — while the rest of the
// bindings stay internal.
export 'src/bindings.dart'
    show
        PdTransportCloseNative,
        PdTransportConnectNative,
        PdTransportWriteNative;
export 'src/custom_methods.dart';
export 'src/enums.dart';
export 'src/library_loader.dart'
    show
        PrinterDriverLibraryNotFound,
        defaultLibraryFileName,
        loadPrinterDriverLibrary,
        printerDriverLibPathVariable;
export 'src/transport.dart';
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

  /// Held for the driver's whole life: pd.h requires a custom transport's `ctx` and
  /// function pointers to stay valid until `pd_destroy`.
  final List<CustomTransport> _transports = <CustomTransport>[];

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

  /// Adds a printer reached over a link the application owns — Bluetooth Classic SPP,
  /// BLE, MFi, a vendor SDK channel, a USB bulk pipe (docs/compatibility-brief.md §25).
  ///
  /// The core drives it exactly as it drives a TCP printer: same ordered fence, same
  /// preflight, same journal, same grading. See [CustomTransport] for the thread
  /// contract and for why the three operations are native pointers.
  ///
  /// [profileId] takes the same ids as [addTcpPrinter]; an unknown one throws rather
  /// than falling back to `generic`, because a caller that asked for a TM-T88VI and
  /// silently got the unknown-device profile would be told a weaker completion story
  /// than it asked for, with nothing in the result explaining why. Zero [widthDots]
  /// means 576.
  ///
  /// The driver retains [transport] until [dispose], which is what keeps its `ctx` and
  /// any Dart close hook alive for as long as the core may call them.
  Printer addCustomPrinter(
    CustomTransport transport, {
    String? profileId,
    int widthDots = 0,
  }) {
    _checkAlive();
    final handle = Arena.using((arena) {
      final vtable =
          arena.allocate<PdTransportVtable>(sizeOf<PdTransportVtable>());
      transport.fillNative(arena, vtable);
      return _bindings.addPrinterCustom(
        _handle,
        vtable,
        transport.ctx,
        arena.string(profileId),
        widthDots,
      );
    });
    if (handle == nullptr) {
      // Nothing took ownership, so the close hook would otherwise outlive its only
      // possible caller — unless this same transport is already driving a printer, in
      // which case releasing it here would pull the trampoline out from under a link
      // the core is still using.
      if (!_transports.contains(transport)) {
        transport.release();
      }
      throw PrinterDriverException(lastError);
    }
    _transports.add(transport);
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

  // --- M15: auto-detection and LAN discovery (docs/api.md §15) ----------------------

  /// Sweeps, identifies and classifies. **Nothing prints and nothing fires.**
  ///
  /// Discovery (the non-printing `DLE EOT 1` sweep) then multi-signal identification per
  /// candidate, then the PRINTLESS subset of the capability probe, respecting the stored
  /// findings cache.
  ///
  /// An ordered fence only means anything when there is print data ahead of it. This
  /// call has none, so the fences go out behind an empty buffer: a device that echoes
  /// them has proved that its firmware *implements* the command, not that the echo waits
  /// for paper to move. The flag is promoted and its provenance is not —
  /// [DetectedCompletion.provenance] stays [Provenance.unverified] on a printless
  /// answer, and the reason appears in [DetectionSummary.degradations]. Full promotion
  /// needs the printing probe or a real job.
  ///
  /// [endpoints] is an explicit `host` / `host:port` list that skips the sweep entirely —
  /// the path for a caller with a known inventory, and the only one that reports an
  /// unreachable address, because it is the only one where somebody named it.
  ///
  /// Blocks this isolate until every candidate is finished.
  List<DetectedPrinter> autoDetect({
    String? subnetCidr,
    List<String> endpoints = const <String>[],
    int port = 0,
    int concurrency = 0,
    Duration? connectTimeout,
    Duration? responseTimeout,
    bool probeUnknown = true,
  }) {
    _checkAlive();
    final count = Arena.using((arena) {
      final options =
          arena.allocate<PdAutoDetectOptions>(sizeOf<PdAutoDetectOptions>());
      options.ref
        ..subnetCidr = arena.string(subnetCidr)
        ..endpoints = _stringArray(arena, endpoints)
        ..port = port
        ..concurrency = concurrency
        ..connectTimeoutMs = connectTimeout?.inMilliseconds ?? 0
        ..responseTimeoutMs = responseTimeout?.inMilliseconds ?? 0
        ..leaveUnknownUnprobed = probeUnknown ? 0 : 1;
      // No callback: a Dart isolate blocked inside an FFI call cannot service a native
      // one until the call returns, so the results are read back by index instead
      // (pd.h, pd_detected_at).
      return _bindings.autoDetect(_handle, options, nullptr, nullptr);
    });
    if (count < 0) {
      throw PrinterDriverException(lastError);
    }
    return Arena.using((arena) {
      final slot =
          arena.allocate<PdDetectedPrinter>(sizeOf<PdDetectedPrinter>());
      final found = <DetectedPrinter>[];
      for (var index = 0; index < count; index++) {
        if (_bindings.detectedAt(_handle, index, slot) != 1) break;
        found.add(DetectedPrinter.fromNative(slot.ref));
      }
      return List<DetectedPrinter>.unmodifiable(found);
    });
  }

  /// [autoDetect] as a stream, for a UI that wants to fill a table as it goes.
  ///
  /// The sweep itself blocks a thread, and Dart cannot deliver a native callback to an
  /// isolate that is blocked inside the call firing it — so this runs the sweep off the
  /// event loop and emits the candidates it found. The stream is honest about that: it
  /// is a stream of results, not a live feed.
  Stream<DetectedPrinter> autoDetectStream({
    String? subnetCidr,
    List<String> endpoints = const <String>[],
    int port = 0,
    int concurrency = 0,
    Duration? connectTimeout,
    Duration? responseTimeout,
    bool probeUnknown = true,
  }) async* {
    final found = await Future(() => autoDetect(
          subnetCidr: subnetCidr,
          endpoints: endpoints,
          port: port,
          concurrency: concurrency,
          connectTimeout: connectTimeout,
          responseTimeout: responseTimeout,
          probeUnknown: probeUnknown,
        ));
    for (final one in found) {
      yield one;
    }
  }

  /// The raw sweep underneath [autoDetect]: is something ESC/POS-shaped listening, and
  /// does its backchannel reach me?
  ///
  /// No identification, no capability probe, no profile — deciding what a device *is*
  /// costs time and belongs to [autoDetect]. The whole write side is `DLE EOT 1`
  /// (`10 04 01`), every byte of which is below 0x20 and therefore cannot print on any
  /// device, ever.
  ///
  /// [subnetCidr] of null sweeps [localSubnet]. Blocks this isolate.
  List<DiscoveredDevice> discover({
    String? subnetCidr,
    int port = 0,
    int concurrency = 0,
    Duration? connectTimeout,
    Duration? responseTimeout,
    bool probeBackchannel = true,
  }) {
    _checkAlive();
    final count = Arena.using((arena) {
      final options =
          arena.allocate<PdDiscoverOptions>(sizeOf<PdDiscoverOptions>());
      options.ref
        ..subnetCidr = arena.string(subnetCidr)
        ..port = port
        ..concurrency = concurrency
        ..connectTimeoutMs = connectTimeout?.inMilliseconds ?? 0
        ..responseTimeoutMs = responseTimeout?.inMilliseconds ?? 0
        ..noBackchannelProbe = probeBackchannel ? 0 : 1;
      return _bindings.discover(_handle, options, nullptr, nullptr);
    });
    if (count < 0) {
      throw PrinterDriverException(lastError);
    }
    return Arena.using((arena) {
      final slot =
          arena.allocate<PdDiscoveredDevice>(sizeOf<PdDiscoveredDevice>());
      final found = <DiscoveredDevice>[];
      for (var index = 0; index < count; index++) {
        if (_bindings.discoveredAt(_handle, index, slot) != 1) break;
        found.add(DiscoveredDevice.fromNative(slot.ref));
      }
      return List<DiscoveredDevice>.unmodifiable(found);
    });
  }

  /// [discover] as a stream. Same caveat as [autoDetectStream].
  Stream<DiscoveredDevice> discoverStream({
    String? subnetCidr,
    int port = 0,
    int concurrency = 0,
    Duration? connectTimeout,
    Duration? responseTimeout,
    bool probeBackchannel = true,
  }) async* {
    final found = await Future(() => discover(
          subnetCidr: subnetCidr,
          port: port,
          concurrency: concurrency,
          connectTimeout: connectTimeout,
          responseTimeout: responseTimeout,
          probeBackchannel: probeBackchannel,
        ));
    for (final one in found) {
      yield one;
    }
  }

  /// The /24 around this host's primary address, as `192.168.1.0/24`, or null when it
  /// cannot be determined.
  ///
  /// Found by asking the routing table which local address would be used to reach a
  /// remote one; no packet is transmitted.
  String? get localSubnet {
    _checkAlive();
    final value = readNativeString(_bindings.localSubnet(_handle));
    return value.isEmpty ? null : value;
  }

  // --- M16: custom method registration (docs/api.md §16) ----------------------------
  //
  // The five extension points, each taking native callbacks for the reason
  // src/custom_methods.dart opens with. A registration lives for the life of the driver:
  // pd.h has no unregister call, and the code the pointers name must stay loaded until
  // [dispose].

  /// Registers a custom completion mechanism.
  ///
  /// Throws [PrinterDriverException] on a bad or duplicate id, or a record the core
  /// refused.
  void registerCompletionMethod(CustomCompletionMethod method) {
    _checkAlive();
    final ok = Arena.using((arena) {
      final native = arena.allocate<PdCompletionMethod>(sizeOf<PdCompletionMethod>());
      native.ref
        ..id = arena.string(method.id)
        ..fenceBytes = method.fenceBytes
        ..matcher = method.matcher
        ..ctx = method.ctx
        ..grade = method.grade.nativeValue
        ..authority = method.authority.nativeValue
        ..methodName = arena.string(method.methodName ?? method.id);
      return _bindings.registerCompletionMethod(_handle, native);
    });
    if (ok != 1) {
      throw PrinterDriverException(lastError);
    }
  }

  /// Registers an extra fingerprinting step for `probe` and [autoDetect].
  ///
  /// Throws [PrinterDriverException] on a bad or duplicate id — and on a request whose
  /// bytes could print, which the core refuses at registration rather than at a venue.
  void registerProbeStep(CustomProbeStep step) {
    _checkAlive();
    final ok = Arena.using((arena) {
      final native = arena.allocate<PdProbeStep>(sizeOf<PdProbeStep>());
      native.ref
        ..id = arena.string(step.id)
        ..requestBytes = arena.bytes(step.requestBytes)
        ..requestSize = step.requestBytes.length
        ..classify = step.classify
        ..ctx = step.ctx;
      return _bindings.registerProbeStep(_handle, native);
    });
    if (ok != 1) {
      throw PrinterDriverException(lastError);
    }
  }

  /// Registers a renderer for a new DSL block kind.
  void registerBlockHandler(CustomBlockHandler handler) {
    _checkAlive();
    final ok = Arena.using((arena) {
      final native = arena.allocate<PdBlockHandler>(sizeOf<PdBlockHandler>());
      native.ref
        ..kind = arena.string(handler.kind)
        ..handler = handler.handler
        ..ctx = handler.ctx;
      return _bindings.registerBlockHandler(_handle, native);
    });
    if (ok != 1) {
      throw PrinterDriverException(lastError);
    }
  }

  /// Registers a template formatter, checked before the built-in table.
  void registerFormatter(CustomFormatter formatter) {
    _checkAlive();
    final ok = Arena.using((arena) {
      final native = arena.allocate<PdFormatter>(sizeOf<PdFormatter>());
      native.ref
        ..name = arena.string(formatter.name)
        ..formatter = formatter.formatter
        ..ctx = formatter.ctx;
      return _bindings.registerFormatter(_handle, native);
    });
    if (ok != 1) {
      throw PrinterDriverException(lastError);
    }
  }

  /// Registers a vendor drawer-kick method.
  void registerDrawerKick(CustomDrawerKick kick) {
    _checkAlive();
    final ok = Arena.using((arena) {
      final native = arena.allocate<PdDrawerKickReg>(sizeOf<PdDrawerKickReg>());
      native.ref
        ..id = arena.string(kick.id)
        ..kickBytes = kick.kickBytes
        ..statusRequest = kick.statusRequest
        ..statusParse = kick.statusParse
        ..ctx = kick.ctx;
      return _bindings.registerDrawerKick(_handle, native);
    });
    if (ok != 1) {
      throw PrinterDriverException(lastError);
    }
  }

  // --- The core's own spelling of a mirrored enum -----------------------------------

  /// The core's own spelling of any mirrored enum value — `pd_job_state_name` and its
  /// fourteen relatives, in one call.
  ///
  /// Not [Enum.name]: that is this package's spelling of a member, and this is the one the
  /// journal records, `pdctl` prints and a support engineer greps for six months later.
  /// Rendering a diagnostic with one and searching the journal for the other is a wasted
  /// afternoon.
  ///
  /// Throws [ArgumentError] for a value that is not one of the ABI's mirrored enums.
  String abiName(Object value) {
    _checkAlive();
    final Pointer<Char> name = switch (value) {
      JobState v => _bindings.jobStateName(v.nativeValue),
      ConfidenceLevel v => _bindings.confidenceLevelName(v.nativeValue),
      DeviceEvent v => _bindings.deviceEventName(v.nativeValue),
      FailureReason v => _bindings.failureReasonName(v.nativeValue),
      JobResult v => _bindings.jobOutcomeName(v.nativeOutcome),
      PayloadKind v => _bindings.payloadKindName(v.nativeValue),
      ConfidenceGrade v => _bindings.confidenceGradeName(v.nativeValue),
      CompletionAuthority v => _bindings.completionAuthorityName(v.nativeValue),
      Provenance v => _bindings.provenanceName(v.nativeValue),
      CommandLanguage v => _bindings.commandLanguageName(v.nativeValue),
      CompletionMechanism v => _bindings.completionMechanismName(v.nativeValue),
      CutVariant v => _bindings.cutVariantName(v.nativeValue),
      DrawerState v => _bindings.drawerStateName(v.nativeValue),
      DrawerPortStandard v => _bindings.drawerPortStandardName(v.nativeValue),
      DrawerKickMethod v => _bindings.drawerKickMethodName(v.nativeValue),
      DrawerStatusMethod v => _bindings.drawerStatusMethodName(v.nativeValue),
      ProfileSelection v => _bindings.profileSelectionName(v.nativeValue),
      DetectionStatus v => _bindings.detectionStatusName(v.nativeValue),
      DrainOrder v => _bindings.drainOrderName(v.nativeValue),
      CompletionMatchKind v => _bindings.matchKindName(v.nativeValue),
      _ => throw ArgumentError.value(value, 'value', 'is not a mirrored ABI enum'),
    };
    return readNativeString(name);
  }

  /// `pd_confidence_grade_letter` — "A+", "A".."E", the letter a report tabulates where
  /// the member name is too long.
  String gradeLetter(ConfidenceGrade grade) {
    _checkAlive();
    return readNativeString(_bindings.confidenceGradeLetter(grade.nativeValue));
  }

  /// A NULL-terminated `char*` array inside [arena], or `nullptr` when empty.
  static Pointer<Pointer<Char>> _stringArray(Arena arena, List<String> values) {
    if (values.isEmpty) return nullptr;
    final array = arena.allocate<Pointer<Char>>(
      (values.length + 1) * sizeOf<Pointer<Char>>(),
    );
    for (var index = 0; index < values.length; index++) {
      array[index] = arena.string(values[index]);
    }
    array[values.length] = nullptr;
    return array;
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

  /// Test support: a scripted printer sitting on the far side of a transport vtable the
  /// caller supplies, for exercising a Bluetooth or vendor-SDK link without hardware.
  ///
  /// The counterpart of [addScriptedPrinterForTesting] for the other half of
  /// docs/compatibility-brief.md §25. That one scripts a device the core reaches on its
  /// own; this one scripts a device the *application* reaches, so a POS integration can
  /// test the transport it wrote — including that its `write` reports short writes
  /// honestly — against a printer that answers the completion fence properly.
  ///
  /// [script] is `ok` (a healthy `GS ( H` printer) or `gsr1` (queued fence only). The
  /// returned link owns a reader thread and is **not** owned by this driver: call
  /// [ScriptedLink.destroy] after [dispose], never before, since the core may still be
  /// writing to it until then.
  ScriptedLink scriptedLinkForTesting({required String script}) {
    _checkAlive();
    if (!_bindings.hasTestSupport) {
      throw PrinterDriverException(
        'this native library exports no pd_test_link_create: it was built from '
        'printerdriver_capi_shared, which by design carries no test doubles. Build '
        'the printerdriver_capi_testing target and point PRINTERDRIVER_LIB_PATH at it.',
      );
    }
    final handle = Arena.using(
      (arena) => _bindings.testLinkCreate(arena.string(script)),
    );
    if (handle == nullptr) {
      throw PrinterDriverException('unknown scripted link id: $script');
    }
    return ScriptedLink._(_bindings, handle);
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

    // Deliberately a turn later. pd_destroy closes every link on its way out, so the
    // last thing a custom transport's close hook ever sees is this call — and that hook
    // is a listener, whose message is queued for this isolate rather than run on the
    // spot. Releasing the trampoline here, synchronously, would throw that queued
    // message away and lose the one notification an application uses to hand its
    // CoreBluetooth peripheral or MFi session back. The core can no longer call any of
    // them, so waiting costs nothing but the turn.
    final transports = List<CustomTransport>.of(_transports);
    _transports.clear();
    if (transports.isNotEmpty) {
      Timer.run(() {
        for (final transport in transports) {
          transport.release();
        }
      });
    }
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

  /// What [completion] is worth before anything has been printed
  /// (docs/compatibility-brief.md §28).
  ///
  /// Not a confidence level, and it changes nothing about what a job reports: it answers
  /// the earlier question of whether this printer's fence should be trusted on the
  /// strength of a profile alone. [Provenance.documented] means the manufacturer's own
  /// command documentation lists the mechanism — in the shipped database, Epson and
  /// nobody else. [Provenance.unverified] is what "ESC/POS compatible" on a datasheet
  /// amounts to, and is the answer that makes running a probe against a site's actual
  /// hardware worth the trip.
  Provenance get completionProvenance {
    _driver._checkAlive();
    return Provenance.fromNative(
        _bindings.printerCompletionProvenance(_handle));
  }

  /// The language this printer's profile is driven in.
  ///
  /// Anything but [CommandLanguage.escPos] is refused with
  /// [FailureReason.unsupported] before a byte is written, so this is readable up front
  /// rather than discovered from a failed job — or, worse, from a label roll spooled
  /// with ESC/POS.
  CommandLanguage get language {
    _driver._checkAlive();
    return CommandLanguage.fromNative(_bindings.printerLanguage(_handle));
  }

  /// Delivers bytes the link received, for a printer added with
  /// [PrinterDriver.addCustomPrinter].
  ///
  /// Safe from any isolate and at any time, including while a job is being written —
  /// that is the normal case, a status answer arriving as the next chunk goes out.
  /// pd.h's one restriction, that this must never be called from inside the transport's
  /// connect/write/close, cannot be violated from Dart: see [CustomTransport].
  ///
  /// False means nothing was listening — the core has not connected yet, has already
  /// closed, or this printer is not a custom transport at all. That is information
  /// rather than an error: bytes arriving with no reader are dropped, exactly as they
  /// would be on a socket nobody is reading.
  bool feedBytes(List<int> bytes) {
    _driver._checkAlive();
    if (bytes.isEmpty) return false;
    return Arena.using(
          (arena) => _bindings.transportFeedBytes(
            _handle,
            arena.bytes(bytes),
            bytes.length,
          ),
        ) !=
        0;
  }

  /// Reports that the link dropped for a reason other than an explicit close: the peer
  /// went out of range, the OS tore the channel down, pairing was revoked.
  ///
  /// Surfaces as [DeviceEvent.connectionLost] and fails any job waiting on a fence
  /// instead of leaving it to time out — the difference between an operator learning
  /// now and learning after the completion budget expires. False when there was no live
  /// transport to notify.
  bool reportLinkDropped([String? message]) {
    _driver._checkAlive();
    return Arena.using(
          (arena) =>
              _bindings.transportLinkDropped(_handle, arena.string(message)),
        ) !=
        0;
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

  /// Pulses the cash drawer and tells the caller nothing.
  ///
  /// Kept because it is ABI, and because a caller that genuinely does not want to wait is
  /// entitled to say so. Everything new should use [openDrawer], which reports what
  /// actually happened.
  void openCashDrawer() {
    _driver._checkAlive();
    _bindings.openCashDrawer(_driver._handle, _handle);
  }

  // --- M14: cash drawer (docs/cash-drawer.md) --------------------------------------

  /// What this printer's drawer port is, and what is known about it.
  DrawerCapabilities get drawerCapabilities {
    _driver._checkAlive();
    return DrawerCapabilities.fromNative(
      _bindings.printerDrawerCapabilities(_handle),
    );
  }

  /// Fires the drawer and reports what was observed, never a boolean.
  ///
  /// The sequence: read the switch first (a drawer that is already out is not pulsed
  /// again), take the printer's peripheral lane so the pulse cannot land inside a fenced
  /// job, emit one queued pulse, watch the switch for the profile's verification window,
  /// and hold the manufacturer cooldown before the next one.
  ///
  /// Refused — zero bytes written, [DrawerState.unknown] — when this model has no drawer
  /// port, when its kick method is one this engine does not drive, or when the electrical
  /// standard is [DrawerPortStandard.unknown]. RJ11/RJ12-looking drawer connectors are not
  /// a universal electrical standard, and an unclassified port is never energised.
  ///
  /// [channel] is 1 for drive 1 (Epson pin 2) and 2 for drive 2 (pin 5), clamped to what
  /// the profile documents; [pulseMs] of 0 uses the profile's own default. Blocks this
  /// isolate until the sequence reaches a verdict.
  DrawerResult openDrawer({int channel = 1, int pulseMs = 0}) {
    _driver._checkAlive();
    return Arena.using((arena) {
      final request = arena.allocate<PdDrawerRequest>(sizeOf<PdDrawerRequest>());
      request.ref
        ..channel = channel
        ..pulseMs = pulseMs;
      return DrawerResult.fromNative(
        _bindings.drawerOpen(_driver._handle, _handle, request),
      );
    });
  }

  /// Reads the drawer switch without firing anything.
  ///
  /// Safe on hardware [openDrawer] refuses, which is what makes it the first half of the
  /// calibration procedure. Blocks this isolate for up to [timeout] (1.5 s when null).
  DrawerReading readDrawerSensor({Duration? timeout}) {
    _driver._checkAlive();
    return DrawerReading.fromNative(
      _bindings.drawerReadSensor(
        _driver._handle,
        _handle,
        timeout?.inMilliseconds ?? 0,
      ),
    );
  }

  /// Records which sense level means "open" for the drawer attached to this printer, and
  /// persists it in the driver's storage directory.
  ///
  /// The procedure is an operator's: ask for the drawer to be closed, read the sensor, ask
  /// for it to be opened, read again, and pass the level observed while it was open.
  /// Returns false when it applies to this process only — an in-memory driver, or a
  /// storage directory that cannot be written.
  bool calibrateDrawerPolarity({required bool highMeansOpen}) {
    _driver._checkAlive();
    return _bindings.drawerCalibratePolarity(
          _driver._handle,
          _handle,
          highMeansOpen ? 1 : 0,
        ) ==
        1;
  }

  /// Whether a polarity has been measured for this printer. Until it has, a sensor
  /// reading carries a level and no interpretation.
  bool get drawerPolarityCalibrated {
    _driver._checkAlive();
    return _bindings.drawerPolarityCalibrated(_driver._handle, _handle) == 1;
  }

  /// Meaningful only while [drawerPolarityCalibrated] is true.
  bool get drawerHighMeansOpen {
    _driver._checkAlive();
    return _bindings.drawerHighMeansOpen(_driver._handle, _handle) == 1;
  }

  // --- M15: the self-test (docs/api.md §15) ---------------------------------------

  /// Prints ONE diagnostic ticket through the full fenced engine and reports what it
  /// established. **This uses paper: the paper is the report.**
  ///
  /// Identity, profile and how it was selected, media, completion mechanism with its
  /// grade ceiling and provenance, the drawer classification, a Czech/Hungarian/Polish
  /// charset line, a Code 128 sample and the job's own verification token in the trailer
  /// QR. Anything the profile cannot draw is printed as a declared degradation rather
  /// than dropped, and repeated in [DetectionSummary.degradations].
  ///
  /// The printer's OWN built-in self-test (`GS ( A`) is a different document and stays
  /// separately reachable through `pdctl test-print`: vendor firmware's view of the
  /// device against this, the SDK's.
  ///
  /// Blocks this isolate until the job is terminal.
  SelfTestResult selfTest({
    String? key,
    bool refreshIdentity = false,
    bool probeWithoutPrinting = false,
    bool barcode = true,
    String? barcodeData,
    bool printVerificationId = true,
    Duration? timeout,
  }) {
    _driver._checkAlive();
    return Arena.using((arena) {
      final options =
          arena.allocate<PdSelfTestOptions>(sizeOf<PdSelfTestOptions>());
      options.ref
        ..key = arena.string(key)
        ..refreshIdentity = refreshIdentity ? 1 : 0
        ..probeWithoutPrinting = probeWithoutPrinting ? 1 : 0
        ..noBarcode = barcode ? 0 : 1
        ..barcodeData = arena.string(barcodeData)
        ..noVerificationId = printVerificationId ? 0 : 1
        ..timeoutMs = timeout?.inMilliseconds ?? 0;

      final out = arena.allocate<PdSelfTestResult>(sizeOf<PdSelfTestResult>());
      if (_bindings.selfTest(_driver._handle, _handle, options, out) != 1) {
        throw PrinterDriverException(_driver.lastError);
      }
      final token = readNativeString(out.ref.printToken);
      final ticket = readNativeString(out.ref.ticketText);
      return SelfTestResult(
        result: jobResultFromNative(out.ref.result),
        detection: DetectionSummary.fromNative(out.ref.detection),
        key: readNativeString(out.ref.key),
        verificationId: token.isEmpty ? null : token,
        ticketLines: ticket.isEmpty
            ? const <String>[]
            : List.unmodifiable(ticket.split('\n')),
      );
    });
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

  // --- M14: cash drawer (docs/cash-drawer.md) --------------------------------------

  /// How many `ESC p` pulses actually reached the device — the number a refusal test
  /// has to watch stay at zero.
  int get drawerKicks {
    _printer._driver._checkAlive();
    return _printer._bindings.testDrawerKicks(_printer._handle);
  }

  /// Where the scripted microswitch currently sits.
  bool get drawerIsOpen {
    _printer._driver._checkAlive();
    return _printer._bindings.testDrawerIsOpen(_printer._handle) == 1;
  }

  /// An operator's hand: moves the drawer without going through the printer, which is
  /// what the non-destructive polarity calibration asks for.
  set drawerIsOpen(bool open) {
    _printer._driver._checkAlive();
    _printer._bindings.testSetDrawerOpen(_printer._handle, open ? 1 : 0);
  }
}

/// Test support: the scripted printer on the far side of a caller-supplied transport.
///
/// Backed by `capi/tests/pd_test_support.h`, which exists only in the library built from
/// the `printerdriver_capi_testing` target. Its three operations have exactly the
/// signatures `pd_transport_vtable` wants, so a test wires them up with
/// [CustomTransport.fromLibrary] and the symbol names below — no forwarding shim.
final class ScriptedLink {
  ScriptedLink._(this._bindings, this._handle);

  /// The symbol implementing `pd_transport_connect_fn` against this link.
  static const String connectSymbol = 'pd_test_link_connect';

  /// The symbol implementing `pd_transport_write_fn` against this link.
  static const String writeSymbol = 'pd_test_link_write';

  /// The symbol implementing `pd_transport_close_fn` against this link.
  static const String closeSymbol = 'pd_test_link_close';

  final PrinterDriverBindings _bindings;
  final Pointer<PdTestLink> _handle;

  bool _destroyed = false;

  /// The `ctx` to register the transport with: the link is what the three operations
  /// act on.
  Pointer<Void> get ctx => _handle.cast<Void>();

  /// Points the link's reader thread at [printer], so the responses it produces reach
  /// the core.
  ///
  /// Call it after [PrinterDriver.addCustomPrinter]. The thread is the point: the
  /// answers arrive from somewhere other than the thread that called `write`, which is
  /// both what pd.h requires and what every real stack does.
  void bindTo(Printer printer) {
    _checkAlive();
    _bindings.testLinkBind(_handle, printer._handle);
  }

  /// Makes the next connect fail, as an unpaired or out-of-range device would.
  void refuseConnections() {
    _checkAlive();
    _bindings.testLinkRefuseConnections(_handle);
  }

  /// How many times the transport was asked to open the link.
  int get connects => _count(_bindings.testLinkConnects);

  /// How many times it was asked to close it.
  int get closes => _count(_bindings.testLinkCloses);

  /// Every byte the vtable's `write` was handed, framing included.
  int get bytesWritten => _count(_bindings.testLinkBytesWritten);

  /// How many cuts the scripted printer performed.
  int get cuts => _count(_bindings.testLinkCuts);

  /// Whether [needle] appears anywhere in what the scripted printer received.
  bool received(String needle) {
    _checkAlive();
    return Arena.using(
          (arena) =>
              _bindings.testLinkReceivedContains(_handle, arena.string(needle)),
        ) !=
        0;
  }

  /// Stops the reader thread and frees the link. Idempotent.
  ///
  /// Only after the driver that used it has been disposed: until then the core may
  /// still call `write` on it, and the counters above are the evidence a test asserts
  /// on afterwards.
  void destroy() {
    if (_destroyed) return;
    _destroyed = true;
    _bindings.testLinkDestroy(_handle);
  }

  int _count(int Function(Pointer<PdTestLink>) read) {
    _checkAlive();
    return read(_handle);
  }

  void _checkAlive() {
    if (_destroyed) {
      throw StateError('this ScriptedLink has been destroyed');
    }
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

// =====================================================================================
// M13b: the print-queue addon (docs/sdk-spec.md §12), through `pd_queue_*`
// =====================================================================================

/// In what order a lane's waiting jobs are chosen.
enum DrainOrder {
  /// Submission order. The safe default for tickets that must stay in sequence.
  fifo(0),

  /// Higher [QueueOptions.priority] first, submission order within equal priorities.
  priority(1);

  const DrainOrder(this.nativeValue);

  final int nativeValue;
}

/// What the queue does with a job it cannot send yet.
final class QueuePolicy {
  const QueuePolicy({
    this.holdWhileOffline = true,
    this.defaultTimeToLive,
    this.maxDepth = 64,
    this.drainOrder = DrainOrder.fifo,
  });

  /// Park jobs while the printer is known to be offline, coverless or out of paper,
  /// instead of failing them one at a time. False makes the queue a pure serializer.
  final bool holdWhileOffline;

  /// Shelf life for a held job. Null means it never expires — and a kitchen ticket must
  /// not print into a recovered kitchen half an hour late.
  final Duration? defaultTimeToLive;

  /// Held jobs per printer. 0 is unlimited, which recreates the printer's own buffer
  /// problem one layer up.
  final int maxDepth;

  final DrainOrder drainOrder;
}

/// Per-job queue settings. The printing half mirrors [JobOptions].
final class QueueOptions {
  const QueueOptions({
    this.key,
    this.timeToLive,
    this.priority = 0,
    this.cut = CutSetting.profile,
    this.openDrawer = false,
    this.preflight = PreflightMode.strict,
    this.timeout,
  });

  /// The idempotency key. A key that already has a job — held, printing, or finished
  /// months ago — returns that job and prints nothing.
  final String? key;

  /// Null uses [QueuePolicy.defaultTimeToLive].
  final Duration? timeToLive;

  /// Orders the waiting set only. A job already in flight is never preempted.
  final int priority;

  final CutSetting cut;
  final bool openDrawer;
  final PreflightMode preflight;

  /// Null uses the profile's completion timeout.
  final Duration? timeout;
}

/// A policy queue in front of one [PrinterDriver].
///
/// Layered on the public API, never part of it. The core already contains the only queue
/// correctness requires — one active job per printer, with a completion fence between
/// jobs. Everything here is policy: holding, draining on recovery, expiry, priority,
/// depth limits.
///
/// Three rules from docs/sdk-spec.md §12 are load-bearing, and none of them is
/// implemented in Dart: they live in the C++ addon behind the ABI, so this behaves
/// identically to the Swift and .NET surfaces.
///
///  1. **A queue is not a retry engine.** A job that ends [JobOutcome.unknown] blocks its
///     printer's lane, and nothing further drains onto that printer until [unblock] is
///     called by somebody who has looked at the paper.
///  2. **Idempotency keys flow through**, all the way into the driver's own index.
///  3. **No bypass.** Draining runs the identical engine path [Printer.print] takes.
///
/// Call [dispose] before disposing the driver.
final class PrintQueue {
  PrintQueue(this._driver, {QueuePolicy policy = const QueuePolicy()}) {
    _driver._checkAlive();
    _handle = Arena.using((arena) {
      final native = arena.allocate<PdQueuePolicy>(sizeOf<PdQueuePolicy>());
      native.ref
        ..holdWhileOffline = policy.holdWhileOffline ? 1 : 0
        ..defaultTtlMs = policy.defaultTimeToLive?.inMilliseconds ?? 0
        ..maxDepth = policy.maxDepth
        ..drainOrder = policy.drainOrder.nativeValue;
      return _driver._bindings.queueCreate(_driver._handle, native);
    });
    if (_handle == nullptr) {
      throw PrinterDriverException(_driver.lastError);
    }
  }

  final PrinterDriver _driver;
  late final Pointer<PdQueue> _handle;
  bool _disposed = false;

  /// Enqueues a job.
  ///
  /// The returned [PrintJob] is an ordinary job handle — same id, same event stream —
  /// already sent when the printer is usable and its lane is free, otherwise held, or
  /// already terminal with [FailureReason.queueOverflow] when the lane is full.
  PrintJob enqueue(
    Printer printer,
    Payload payload, {
    QueueOptions options = const QueueOptions(),
  }) {
    _checkAlive();
    final handle = Arena.using((arena) {
      final nativePayload = arena.allocate<PdPayload>(sizeOf<PdPayload>());
      payload.fillNative(arena, nativePayload);
      final nativeOptions =
          arena.allocate<PdQueueOptions>(sizeOf<PdQueueOptions>());
      nativeOptions.ref
        ..key = options.key == null ? nullptr : arena.string(options.key!)
        ..ttlMs = options.timeToLive?.inMilliseconds ?? 0
        ..priority = options.priority
        ..cut = options.cut.nativeValue
        ..openDrawer = options.openDrawer ? 1 : 0
        ..preflight = options.preflight.nativeValue
        ..timeoutMs = options.timeout?.inMilliseconds ?? 0;
      return _driver._bindings
          .queueEnqueue(_handle, printer._handle, nativePayload, nativeOptions);
    });
    if (handle == nullptr) {
      throw PrinterDriverException(_driver.lastError);
    }
    return _driver._internJob(handle);
  }

  /// Operator hold, independent of what the device is reporting.
  void pause(String printerId) => _withId(printerId, _driver._bindings.queuePause);
  void resume(String printerId) => _withId(printerId, _driver._bindings.queueResume);
  bool isPaused(String printerId) =>
      _queryId(printerId, _driver._bindings.queueIsPaused) != 0;

  /// True once a job on this printer ended [JobOutcome.unknown]. Rule 1: nothing more
  /// drains onto that lane until a person has looked at the paper and called [unblock].
  bool isBlocked(String printerId) =>
      _queryId(printerId, _driver._bindings.queueIsBlocked) != 0;
  void unblock(String printerId) => _withId(printerId, _driver._bindings.queueUnblock);

  /// Held jobs. Omit [printerId] to count every lane.
  int pending([String? printerId]) {
    _checkAlive();
    if (printerId == null) {
      return _driver._bindings.queuePending(_handle, nullptr);
    }
    return _queryId(printerId, _driver._bindings.queuePending);
  }

  int get expiredCount => _count(_driver._bindings.queueExpiredCount);
  int get overflowCount => _count(_driver._bindings.queueOverflowCount);
  int get drainedCount => _count(_driver._bindings.queueDrainedCount);

  /// Runs one expiry-and-drain pass on the calling isolate's thread. The queue's own
  /// thread already does this on every device event and whenever a TTL comes due.
  void tick() {
    _checkAlive();
    _driver._bindings.queueTick(_handle);
  }

  /// Stops the queue thread and frees the handle. Held jobs stay held and stay
  /// non-terminal: the queue does not invent an outcome for a job whose fate it does not
  /// know. Must run before [PrinterDriver.dispose].
  void dispose() {
    if (_disposed) {
      return;
    }
    _disposed = true;
    _driver._bindings.queueDestroy(_handle);
  }

  void _checkAlive() {
    if (_disposed) {
      throw StateError('this PrintQueue has been disposed');
    }
    _driver._checkAlive();
  }

  void _withId(String printerId, void Function(Pointer<PdQueue>, Pointer<Char>) call) {
    _checkAlive();
    Arena.using((arena) => call(_handle, arena.string(printerId)));
  }

  int _queryId(String printerId, int Function(Pointer<PdQueue>, Pointer<Char>) call) {
    _checkAlive();
    return Arena.using((arena) => call(_handle, arena.string(printerId)));
  }

  int _count(int Function(Pointer<PdQueue>) call) {
    _checkAlive();
    return call(_handle);
  }
}
