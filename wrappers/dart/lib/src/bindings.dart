/// Hand-written `dart:ffi` bindings for `capi/include/printerdriver/pd.h`.
///
/// Hand-written rather than generated: pd.h is a small, deliberately closed surface
/// whose comments carry the contract (thread rules, string ownership, handle lifetime),
/// and a generator reproduces the declarations while dropping exactly that. Everything
/// in this file is a one-for-one transcription of the header — no defaults, no
/// validation, no policy. The layer above (`package:printerdriver`) owns ergonomics; the
/// core owns every decision.
///
/// Struct layouts follow the C rules of natural alignment, which is what `dart:ffi`
/// applies to a [Struct] as well, so the field order here is the field order in pd.h and
/// must stay that way. `test/abi_layout_test.dart` pins the resulting sizes.
library;

import 'dart:ffi';

// --- Opaque handles ---------------------------------------------------------------
//
// Owned by their driver and freed by pd_destroy; never freed individually. The same
// underlying job always maps to the same pd_job pointer, which is what makes
// idempotency-key dedupe observable as pointer equality.

/// `pd_driver`
final class PdDriver extends Opaque {}

/// `pd_printer`
final class PdPrinter extends Opaque {}

/// `pd_job`
final class PdJob extends Opaque {}

// --- Structs ------------------------------------------------------------------------

/// `pd_job_event` — one step of a job's history.
final class PdJobEvent extends Struct {
  @Int32()
  external int state;

  @Int32()
  external int confidence;

  /// 0 for every non-failure transition, in which case [reason] is `PD_REASON_NONE`.
  @Int32()
  external int hasReason;

  @Int32()
  external int reason;

  /// From a steady clock: the wall clock may step, job timing must not.
  @Uint64()
  external int monotonicMs;
}

/// `pd_job_result` — the terminal answer, tri-state by construction.
final class PdJobResult extends Struct {
  @Int32()
  external int outcome;

  /// Carried on all three outcomes, not only on Done.
  @Int32()
  external int confidence;

  @Int32()
  external int reason;
}

/// `pd_device_status` — last known device state, never a live query.
///
/// Every field other than [connected] and [observed] is `PD_UNKNOWN` (-1), `PD_FALSE`
/// (0) or `PD_TRUE` (1).
final class PdDeviceStatus extends Struct {
  @Int32()
  external int connected;

  @Int32()
  external int observed;

  @Int32()
  external int online;

  @Int32()
  external int coverOpen;

  @Int32()
  external int paperOut;

  @Int32()
  external int paperNearEnd;

  @Int32()
  external int cutterError;

  @Int32()
  external int unrecoverableError;

  @Int32()
  external int recoverableError;
}

/// `pd_raster_rgba8` — tier 1.
final class PdRasterRgba8 extends Struct {
  external Pointer<Uint8> pixels;

  @Uint32()
  external int width;

  @Uint32()
  external int height;

  /// 0 for tightly packed rows.
  @Uint32()
  external int strideBytes;

  @Int32()
  external int binarization;

  /// Only read for `PD_BINARIZATION_FIXED_THRESHOLD`; 0 means 128.
  @Uint8()
  external int threshold;

  /// 0 means 1024, the Epson tall-image split.
  @Uint32()
  external int maxRowsPerBand;
}

/// `pd_op` — one operation of the document tier.
final class PdOp extends Struct {
  @Int32()
  external int kind;

  /// UTF-8; transliterated to the document's code page, lossily.
  external Pointer<Char> text;

  @Int32()
  external int value;
}

/// `pd_document` — tier 2.
final class PdDocument extends Struct {
  external Pointer<PdOp> ops;

  @Size()
  external int count;

  @Int32()
  external int codePage;
}

/// `pd_raw` — tier 3, passed through verbatim.
final class PdRaw extends Struct {
  external Pointer<Uint8> bytes;

  @Size()
  external int size;
}

/// The anonymous union inside `pd_payload`.
final class PdPayloadAs extends Union {
  external PdRasterRgba8 raster;
  external PdDocument document;
  external PdRaw raw;
}

/// `pd_payload`
final class PdPayload extends Struct {
  @Int32()
  external int kind;

  external PdPayloadAs as;
}

/// `pd_config` — all-zeroes is the documented default.
final class PdConfig extends Struct {
  /// NULL or "" means in-memory: no journal, no recovery.
  external Pointer<Char> storageDirectory;

  /// 0 keeps the durability rule; 1 is for tests only.
  @Int32()
  external int fsyncDisabled;

  external Pointer<NativeFunction<PdLogCbNative>> log;

  external Pointer<Void> logCtx;
}

/// `pd_tcp_config`
final class PdTcpConfig extends Struct {
  /// NULL or "" derives one from the endpoint.
  external Pointer<Char> printerId;

  external Pointer<Char> host;

  /// 0 means 9100.
  @Uint16()
  external int port;

  /// 0 means 576.
  @Uint32()
  external int widthDots;

  /// NULL or "" means "generic"; see `pd_profile_ids`.
  external Pointer<Char> profileId;

  /// 0 means 3000.
  @Uint32()
  external int connectTimeoutMs;
}

/// `pd_job_options`
final class PdJobOptions extends Struct {
  /// NULL or "" generates one, which means no dedupe protection.
  external Pointer<Char> key;

  @Int32()
  external int cut;

  @Int32()
  external int openDrawer;

  @Int32()
  external int preflight;

  /// 0 means the profile's completion timeout.
  @Uint32()
  external int timeoutMs;
}

// --- Callbacks ----------------------------------------------------------------------
//
// pd.h: callbacks run on the core's threads, not the caller's. Job events arrive on the
// owning printer's worker thread — except the replay of already-recorded events, which
// runs on the thread calling pd_subscribe_job, before that call returns. Device events
// arrive on whichever thread decoded the status. A callback must not block and must not
// call back into the same driver.
//
// For Dart that has one consequence worth stating in the bindings themselves: the
// `const pd_job_event*` handed to a job callback points at a temporary that dies when
// the call returns, so a NativeCallable.listener — which runs the Dart function after
// the native call has already returned — must never dereference it. See
// PrintJob in ../printerdriver.dart for how the event data is obtained instead.
// Device events carry their payload by value and have no such constraint.

/// `pd_job_event_cb`
typedef PdJobEventCbNative = Void Function(
  Pointer<PdJob> job,
  Pointer<PdJobEvent> event,
  Pointer<Void> ctx,
);

/// `pd_device_event_cb`
typedef PdDeviceEventCbNative = Void Function(
  Pointer<PdPrinter> printer,
  Int32 event,
  Pointer<Void> ctx,
);

/// `pd_log_cb`
typedef PdLogCbNative = Void Function(Pointer<Char> message, Pointer<Void> ctx);

// --- Native signatures --------------------------------------------------------------

typedef _PdCreateNative = Pointer<PdDriver> Function(Pointer<PdConfig>);
typedef _PdDestroyNative = Void Function(Pointer<PdDriver>);
typedef _PdLastErrorNative = Pointer<Char> Function(Pointer<PdDriver>);
typedef _PdProfileIdsNative = Pointer<Pointer<Char>> Function();

typedef _PdAddPrinterTcpNative = Pointer<PdPrinter> Function(
    Pointer<PdDriver>, Pointer<PdTcpConfig>);
typedef _PdPrinterIdNative = Pointer<Char> Function(Pointer<PdPrinter>);
typedef _PdPrinterWidthDotsNative = Uint32 Function(Pointer<PdPrinter>);
typedef _PdPrinterCompletionNative = Int32 Function(Pointer<PdPrinter>);
typedef _PdPrinterStatusNative = PdDeviceStatus Function(
    Pointer<PdDriver>, Pointer<PdPrinter>);
typedef _PdPrinterRefreshStatusNative = PdDeviceStatus Function(
    Pointer<PdDriver>, Pointer<PdPrinter>, Uint32);
typedef _PdOpenCashDrawerNative = Void Function(
    Pointer<PdDriver>, Pointer<PdPrinter>);
typedef _PdPrinterDrainNative = Void Function(
    Pointer<PdDriver>, Pointer<PdPrinter>);
typedef _PdSubscribeDeviceNative = Void Function(
    Pointer<PdDriver>,
    Pointer<PdPrinter>,
    Pointer<NativeFunction<PdDeviceEventCbNative>>,
    Pointer<Void>);

typedef _PdPrintNative = Pointer<PdJob> Function(Pointer<PdDriver>,
    Pointer<PdPrinter>, Pointer<PdPayload>, Pointer<PdJobOptions>);
typedef _PdForceReprintNative = Pointer<PdJob> Function(Pointer<PdDriver>,
    Pointer<PdPrinter>, Pointer<Char>, Pointer<PdJobOptions>);
typedef _PdFindJobNative = Pointer<PdJob> Function(
    Pointer<PdDriver>, Pointer<Char>);
typedef _PdJobIdNative = Pointer<Char> Function(Pointer<PdJob>);
typedef _PdJobAttemptNative = Uint32 Function(Pointer<PdJob>);
typedef _PdJobCurrentStateNative = Int32 Function(Pointer<PdJob>);
typedef _PdJobConfidenceNative = Int32 Function(Pointer<PdJob>);
typedef _PdJobIsTerminalNative = Int32 Function(Pointer<PdJob>);
typedef _PdSubscribeJobNative = Void Function(Pointer<PdDriver>, Pointer<PdJob>,
    Pointer<NativeFunction<PdJobEventCbNative>>, Pointer<Void>);
typedef _PdJobAwaitNative = Int32 Function(
    Pointer<PdDriver>, Pointer<PdJob>, Uint32, Pointer<PdJobResult>);

typedef _PdEnumNameNative = Pointer<Char> Function(Int32);
typedef _PdCodePageAtNative = Int32 Function(Int32);

// Test support (capi/tests/pd_test_support.h). Present only in the library built from
// the printerdriver_capi_testing target; the shipped library has none of these symbols,
// which is the point of the split.
typedef _PdAddPrinterScriptedNative = Pointer<PdPrinter> Function(
    Pointer<PdDriver>, Pointer<Char>, Pointer<Char>);
typedef _PdTestPrintDataBytesNative = Size Function(Pointer<PdPrinter>);
typedef _PdTestCutsNative = Size Function(Pointer<PdPrinter>);
typedef _PdTestReceivedContainsNative = Int Function(
    Pointer<PdPrinter>, Pointer<Char>);
typedef _PdTestCppEnumCountNative = Int Function(Int32);
typedef _PdTestCppEnumNameNative = Pointer<Char> Function(Int32, Int);
typedef _PdTestCppEnumValueNative = Int Function(Int32, Int);
typedef _PdTestEnumLabelNative = Pointer<Char> Function(Int32);

/// `pd_test_enum` — the enum-bridge ids of `pd_test_support.h`.
enum PdTestEnum {
  jobState(0),
  confidence(1),
  deviceEvent(2),
  failureReason(3),
  jobOutcome(4),
  cut(5),
  preflight(6),
  payloadKind(7),
  completion(8),
  cutVariant(9),
  alignment(10),
  codePage(11),
  binarization(12);

  const PdTestEnum(this.nativeValue);

  final int nativeValue;

  /// `PD_TEST_ENUM_TOTAL`: the sentinel, not a member.
  static const int total = 13;
}

/// Every `pd_*` entry point of pd.h, resolved from one [DynamicLibrary].
final class PrinterDriverBindings {
  PrinterDriverBindings(DynamicLibrary library)
      : _library = library,
        create = library.lookupFunction<_PdCreateNative,
            Pointer<PdDriver> Function(Pointer<PdConfig>)>('pd_create'),
        destroy = library.lookupFunction<_PdDestroyNative,
            void Function(Pointer<PdDriver>)>('pd_destroy'),
        lastError = library.lookupFunction<_PdLastErrorNative,
            Pointer<Char> Function(Pointer<PdDriver>)>('pd_last_error'),
        profileIds = library.lookupFunction<_PdProfileIdsNative,
            Pointer<Pointer<Char>> Function()>('pd_profile_ids'),
        addPrinterTcp = library.lookupFunction<
            _PdAddPrinterTcpNative,
            Pointer<PdPrinter> Function(
                Pointer<PdDriver>, Pointer<PdTcpConfig>)>(
          'pd_add_printer_tcp',
        ),
        printerId = library.lookupFunction<_PdPrinterIdNative,
            Pointer<Char> Function(Pointer<PdPrinter>)>('pd_printer_id'),
        printerWidthDots = library.lookupFunction<_PdPrinterWidthDotsNative,
            int Function(Pointer<PdPrinter>)>('pd_printer_width_dots'),
        printerCompletion = library.lookupFunction<_PdPrinterCompletionNative,
            int Function(Pointer<PdPrinter>)>('pd_printer_completion'),
        printerStatus = library.lookupFunction<_PdPrinterStatusNative,
            PdDeviceStatus Function(Pointer<PdDriver>, Pointer<PdPrinter>)>(
          'pd_printer_status',
        ),
        printerRefreshStatus = library.lookupFunction<
            _PdPrinterRefreshStatusNative,
            PdDeviceStatus Function(
                Pointer<PdDriver>, Pointer<PdPrinter>, int)>(
          'pd_printer_refresh_status',
        ),
        openCashDrawer = library.lookupFunction<
            _PdOpenCashDrawerNative,
            void Function(
                Pointer<PdDriver>, Pointer<PdPrinter>)>('pd_open_cash_drawer'),
        printerDrain = library.lookupFunction<
            _PdPrinterDrainNative,
            void Function(
                Pointer<PdDriver>, Pointer<PdPrinter>)>('pd_printer_drain'),
        subscribeDevice = library.lookupFunction<
            _PdSubscribeDeviceNative,
            void Function(Pointer<PdDriver>, Pointer<PdPrinter>,
                Pointer<NativeFunction<PdDeviceEventCbNative>>, Pointer<Void>)>(
          'pd_subscribe_device',
        ),
        print = library.lookupFunction<
            _PdPrintNative,
            Pointer<PdJob> Function(Pointer<PdDriver>, Pointer<PdPrinter>,
                Pointer<PdPayload>, Pointer<PdJobOptions>)>('pd_print'),
        forceReprint = library.lookupFunction<
            _PdForceReprintNative,
            Pointer<PdJob> Function(Pointer<PdDriver>, Pointer<PdPrinter>,
                Pointer<Char>, Pointer<PdJobOptions>)>('pd_force_reprint'),
        findJob = library.lookupFunction<
            _PdFindJobNative,
            Pointer<PdJob> Function(
                Pointer<PdDriver>, Pointer<Char>)>('pd_find_job'),
        jobId = library.lookupFunction<_PdJobIdNative,
            Pointer<Char> Function(Pointer<PdJob>)>('pd_job_id'),
        jobKey = library.lookupFunction<_PdJobIdNative,
            Pointer<Char> Function(Pointer<PdJob>)>('pd_job_key'),
        jobAttempt = library.lookupFunction<_PdJobAttemptNative,
            int Function(Pointer<PdJob>)>('pd_job_attempt'),
        jobCurrentState = library.lookupFunction<_PdJobCurrentStateNative,
            int Function(Pointer<PdJob>)>('pd_job_current_state'),
        jobConfidence = library.lookupFunction<_PdJobConfidenceNative,
            int Function(Pointer<PdJob>)>('pd_job_confidence'),
        jobIsTerminal = library.lookupFunction<_PdJobIsTerminalNative,
            int Function(Pointer<PdJob>)>('pd_job_is_terminal'),
        subscribeJob = library.lookupFunction<
            _PdSubscribeJobNative,
            void Function(Pointer<PdDriver>, Pointer<PdJob>,
                Pointer<NativeFunction<PdJobEventCbNative>>, Pointer<Void>)>(
          'pd_subscribe_job',
        ),
        jobAwait = library.lookupFunction<
            _PdJobAwaitNative,
            int Function(Pointer<PdDriver>, Pointer<PdJob>, int,
                Pointer<PdJobResult>)>('pd_job_await'),
        jobStateName = library.lookupFunction<_PdEnumNameNative,
            Pointer<Char> Function(int)>('pd_job_state_name'),
        confidenceLevelName = library.lookupFunction<_PdEnumNameNative,
            Pointer<Char> Function(int)>('pd_confidence_level_name'),
        deviceEventName = library.lookupFunction<_PdEnumNameNative,
            Pointer<Char> Function(int)>('pd_device_event_name'),
        failureReasonName = library.lookupFunction<_PdEnumNameNative,
            Pointer<Char> Function(int)>('pd_failure_reason_name'),
        jobOutcomeName = library.lookupFunction<_PdEnumNameNative,
            Pointer<Char> Function(int)>('pd_job_outcome_name'),
        payloadKindName = library.lookupFunction<_PdEnumNameNative,
            Pointer<Char> Function(int)>('pd_payload_kind_name'),
        completionMechanismName = library.lookupFunction<_PdEnumNameNative,
            Pointer<Char> Function(int)>('pd_completion_mechanism_name'),
        cutVariantName = library.lookupFunction<_PdEnumNameNative,
            Pointer<Char> Function(int)>('pd_cut_variant_name'),
        codePageAt =
            library.lookupFunction<_PdCodePageAtNative, int Function(int)>(
                'pd_code_page_at');

  final DynamicLibrary _library;

  // --- Driver ---
  final Pointer<PdDriver> Function(Pointer<PdConfig> config) create;
  final void Function(Pointer<PdDriver> driver) destroy;
  final Pointer<Char> Function(Pointer<PdDriver> driver) lastError;
  final Pointer<Pointer<Char>> Function() profileIds;

  // --- Printers ---
  final Pointer<PdPrinter> Function(Pointer<PdDriver>, Pointer<PdTcpConfig>)
      addPrinterTcp;
  final Pointer<Char> Function(Pointer<PdPrinter>) printerId;
  final int Function(Pointer<PdPrinter>) printerWidthDots;
  final int Function(Pointer<PdPrinter>) printerCompletion;
  final PdDeviceStatus Function(Pointer<PdDriver>, Pointer<PdPrinter>)
      printerStatus;
  final PdDeviceStatus Function(Pointer<PdDriver>, Pointer<PdPrinter>, int)
      printerRefreshStatus;
  final void Function(Pointer<PdDriver>, Pointer<PdPrinter>) openCashDrawer;
  final void Function(Pointer<PdDriver>, Pointer<PdPrinter>) printerDrain;
  final void Function(
      Pointer<PdDriver>,
      Pointer<PdPrinter>,
      Pointer<NativeFunction<PdDeviceEventCbNative>>,
      Pointer<Void>) subscribeDevice;

  // --- Jobs ---
  final Pointer<PdJob> Function(Pointer<PdDriver>, Pointer<PdPrinter>,
      Pointer<PdPayload>, Pointer<PdJobOptions>) print;
  final Pointer<PdJob> Function(Pointer<PdDriver>, Pointer<PdPrinter>,
      Pointer<Char>, Pointer<PdJobOptions>) forceReprint;
  final Pointer<PdJob> Function(Pointer<PdDriver>, Pointer<Char>) findJob;
  final Pointer<Char> Function(Pointer<PdJob>) jobId;
  final Pointer<Char> Function(Pointer<PdJob>) jobKey;
  final int Function(Pointer<PdJob>) jobAttempt;
  final int Function(Pointer<PdJob>) jobCurrentState;
  final int Function(Pointer<PdJob>) jobConfidence;
  final int Function(Pointer<PdJob>) jobIsTerminal;
  final void Function(Pointer<PdDriver>, Pointer<PdJob>,
      Pointer<NativeFunction<PdJobEventCbNative>>, Pointer<Void>) subscribeJob;
  final int Function(
      Pointer<PdDriver>, Pointer<PdJob>, int, Pointer<PdJobResult>) jobAwait;

  // --- Enum names ---
  final Pointer<Char> Function(int) jobStateName;
  final Pointer<Char> Function(int) confidenceLevelName;
  final Pointer<Char> Function(int) deviceEventName;
  final Pointer<Char> Function(int) failureReasonName;
  final Pointer<Char> Function(int) jobOutcomeName;
  final Pointer<Char> Function(int) payloadKindName;
  final Pointer<Char> Function(int) completionMechanismName;
  final Pointer<Char> Function(int) cutVariantName;
  final int Function(int) codePageAt;

  /// Whether this library carries `capi/tests/pd_test_support.h`, i.e. whether it was
  /// built from the `printerdriver_capi_testing` target.
  bool get hasTestSupport => _library.providesSymbol('pd_add_printer_scripted');

  /// `pd_add_printer_scripted` — a printer backed by an in-process scripted device.
  ///
  /// Script ids: `ok`, `gsr1`, `silent`, `paperout`, `refuse`. Only ever resolved by
  /// this package's own test suite; the shipped library does not export it, and asking
  /// for it there throws.
  late final Pointer<PdPrinter> Function(
          Pointer<PdDriver>, Pointer<Char>, Pointer<Char>) addPrinterScripted =
      _library.lookupFunction<
          _PdAddPrinterScriptedNative,
          Pointer<PdPrinter> Function(
              Pointer<PdDriver>, Pointer<Char>, Pointer<Char>)>(
    'pd_add_printer_scripted',
  );

  /// `pd_test_print_data_bytes`
  late final int Function(Pointer<PdPrinter>) testPrintDataBytes =
      _library.lookupFunction<_PdTestPrintDataBytesNative,
          int Function(Pointer<PdPrinter>)>(
    'pd_test_print_data_bytes',
  );

  /// `pd_test_cuts`
  late final int Function(Pointer<PdPrinter>) testCuts = _library
      .lookupFunction<_PdTestCutsNative, int Function(Pointer<PdPrinter>)>(
    'pd_test_cuts',
  );

  /// `pd_test_received_contains`
  late final int Function(Pointer<PdPrinter>, Pointer<Char>)
      testReceivedContains = _library.lookupFunction<
          _PdTestReceivedContainsNative,
          int Function(
              Pointer<PdPrinter>, Pointer<Char>)>('pd_test_received_contains');

  /// `pd_test_cpp_enum_count` — how many members the C++ enum has.
  late final int Function(int) testCppEnumCount =
      _library.lookupFunction<_PdTestCppEnumCountNative, int Function(int)>(
    'pd_test_cpp_enum_count',
  );

  /// `pd_test_cpp_enum_name` — the core's own spelling, or NULL when it has none.
  late final Pointer<Char> Function(int, int) testCppEnumName =
      _library.lookupFunction<_PdTestCppEnumNameNative,
          Pointer<Char> Function(int, int)>(
    'pd_test_cpp_enum_name',
  );

  /// `pd_test_cpp_enum_value` — the C++ member's numeric value at that index.
  late final int Function(int, int) testCppEnumValue = _library
      .lookupFunction<_PdTestCppEnumValueNative, int Function(int, int)>(
    'pd_test_cpp_enum_value',
  );

  /// `pd_test_enum_label`
  late final Pointer<Char> Function(int) testEnumLabel = _library
      .lookupFunction<_PdTestEnumLabelNative, Pointer<Char> Function(int)>(
    'pd_test_enum_label',
  );
}
