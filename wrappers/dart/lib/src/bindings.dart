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

/// `pd_test_link` — the scripted far side of a caller-supplied transport vtable.
///
/// From `capi/tests/pd_test_support.h`, so present only in the library built from the
/// `printerdriver_capi_testing` target. Unlike the handles above it is owned by whoever
/// created it, not by a driver.
final class PdTestLink extends Opaque {}

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

  /// What class of evidence the claim rests on — `pd_confidence_grade`.
  @Int32()
  external int grade;

  /// Who made the claim — `pd_completion_authority`.
  @Int32()
  external int authority;

  /// The command behind it, e.g. `GS(H) fn48`. Owned by the `pd_job`, valid until
  /// `pd_destroy`; never NULL, and `"none"` when nothing was confirmed.
  external Pointer<Char> method;
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

// --- Custom transports --------------------------------------------------------------
//
// pd.h: the platform owns the socket, the core owns the protocol. These three are
// invoked on the core's worker thread for the printer, one at a time, never
// concurrently with each other — see the thread contract quoted in `transport.dart`,
// which is also where the reason no Dart function may sit behind connect or write is
// written down.

/// `pd_transport_connect_fn` — non-zero for success, 0 for failure.
typedef PdTransportConnectNative = Int32 Function(Pointer<Void> ctx);

/// `pd_transport_write_fn` — bytes actually transferred, or negative for a hard
/// failure.
///
/// A short write is reported honestly rather than rounded up: zero bytes out is a known
/// failure and one byte out is Unknown (docs/api.md §4).
typedef PdTransportWriteNative = Int64 Function(
  Pointer<Void> ctx,
  Pointer<Uint8> data,
  Size size,
);

/// `pd_transport_close_fn` — called once per successful connect; again is harmless.
typedef PdTransportCloseNative = Void Function(Pointer<Void> ctx);

/// `pd_transport_vtable`
final class PdTransportVtable extends Struct {
  external Pointer<NativeFunction<PdTransportConnectNative>> connect;

  external Pointer<NativeFunction<PdTransportWriteNative>> write;

  /// May be null: the core then simply never closes the link itself.
  external Pointer<NativeFunction<PdTransportCloseNative>> close;

  /// What the printer id and the diagnostics derive from, e.g.
  /// `bt-spp:00:11:22:33:44:55`. Copied before `pd_add_printer_custom` returns; null or
  /// "" becomes `custom`.
  external Pointer<Char> description;
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

  /// Fed before the first content byte.
  @Uint32()
  external int topFeedDots;

  /// The *total* clearance before the cut: the core feeds max(blade clearance, this).
  @Uint32()
  external int bottomFeedDots;

  /// Inverted, so an all-zeroes struct still prints the verification identifier.
  @Int32()
  external int suppressVerificationId;
}

// --- M13b: the print-queue addon (docs/sdk-spec.md §12) -------------------------------

/// `pd_queue` — a policy queue in front of one driver. The one handle in this ABI the
/// caller owns and frees, because it is an addon rather than part of the driver's object
/// graph: `pd_queue_destroy` must run before `pd_destroy`.
final class PdQueue extends Opaque {}

/// `pd_queue_policy`. All-zeroes is a pure serializer: hold nothing, never expire,
/// unlimited depth, FIFO.
final class PdQueuePolicy extends Struct {
  @Int32()
  external int holdWhileOffline;

  /// 0 means a held job never expires.
  @Uint32()
  external int defaultTtlMs;

  /// 0 is unlimited, which recreates the printer's own buffer problem one layer up.
  @Uint32()
  external int maxDepth;

  @Int32()
  external int drainOrder;
}

/// `pd_queue_options`
final class PdQueueOptions extends Struct {
  external Pointer<Char> key;

  /// 0 uses the policy default.
  @Uint32()
  external int ttlMs;

  /// Orders the waiting set only; a job in flight is never preempted.
  @Int32()
  external int priority;

  @Int32()
  external int cut;

  @Int32()
  external int openDrawer;

  @Int32()
  external int preflight;

  @Uint32()
  external int timeoutMs;
}

/// `pd_reprint_options`
final class PdReprintOptions extends Struct {
  external PdJobOptions job;

  /// Inverted, so an all-zeroes struct still prints the reprint banner.
  @Int32()
  external int suppressBanner;
}

// --- Callbacks ----------------------------------------------------------------------
//
// pd.h: callbacks run on the core's threads, not the caller's. Job events arrive on the
// owning printer's worker thread — except the replay of already-recorded events, which
// runs on the thread calling pd_subscribe_job, before that call returns. Device events
// arrive on whichever thread decoded the status. A callback must not block and must not
// call back into the same driver.
//
// Both callbacks carry their payload **by value**, which is what makes them usable from
// a NativeCallable.listener: the listener runs the Dart function after the native call
// has already returned, so anything reached through a pointer into the emitting
// worker's frame would be gone by then. A copy has no such lifetime.

/// `pd_job_event_cb`
typedef PdJobEventCbNative = Void Function(
  Pointer<PdJob> job,
  PdJobEvent event,
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
typedef _PdAddPrinterCustomNative = Pointer<PdPrinter> Function(
    Pointer<PdDriver>,
    Pointer<PdTransportVtable>,
    Pointer<Void>,
    Pointer<Char>,
    Uint32);
typedef _PdTransportFeedBytesNative = Int32 Function(
    Pointer<PdPrinter>, Pointer<Uint8>, Size);
typedef _PdTransportLinkDroppedNative = Int32 Function(
    Pointer<PdPrinter>, Pointer<Char>);
typedef _PdPrinterIdNative = Pointer<Char> Function(Pointer<PdPrinter>);
typedef _PdPrinterWidthDotsNative = Uint32 Function(Pointer<PdPrinter>);
typedef _PdPrinterCompletionNative = Int32 Function(Pointer<PdPrinter>);
typedef _PdPrinterProvenanceNative = Int32 Function(Pointer<PdPrinter>);
typedef _PdPrinterLanguageNative = Int32 Function(Pointer<PdPrinter>);
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
typedef _PdForceReprintOptsNative = Pointer<PdJob> Function(Pointer<PdDriver>,
    Pointer<PdPrinter>, Pointer<Char>, Pointer<PdReprintOptions>);
typedef _PdFindJobNative = Pointer<PdJob> Function(
    Pointer<PdDriver>, Pointer<Char>);
typedef _PdJobByTokenNative = Pointer<PdJob> Function(
    Pointer<PdDriver>, Pointer<Char>);
typedef _PdInstanceNonceNative = Pointer<Char> Function(Pointer<PdDriver>);
typedef _PdJobIdNative = Pointer<Char> Function(Pointer<PdJob>);
typedef _PdJobAttemptNative = Uint32 Function(Pointer<PdJob>);
typedef _PdJobCurrentStateNative = Int32 Function(Pointer<PdJob>);
typedef _PdJobConfidenceNative = Int32 Function(Pointer<PdJob>);
typedef _PdJobIsTerminalNative = Int32 Function(Pointer<PdJob>);
typedef _PdSubscribeJobNative = Void Function(Pointer<PdDriver>, Pointer<PdJob>,
    Pointer<NativeFunction<PdJobEventCbNative>>, Pointer<Void>);
typedef _PdJobAwaitNative = Int32 Function(
    Pointer<PdDriver>, Pointer<PdJob>, Uint32, Pointer<PdJobResult>);

// M14 — cash drawer (docs/cash-drawer.md).
typedef _PdPrinterDrawerCapabilitiesNative = PdDrawerCapabilities Function(
    Pointer<PdPrinter>);
typedef _PdDrawerOpenNative = PdDrawerResult Function(
    Pointer<PdDriver>, Pointer<PdPrinter>, Pointer<PdDrawerRequest>);
typedef _PdDrawerReadSensorNative = PdDrawerReading Function(
    Pointer<PdDriver>, Pointer<PdPrinter>, Uint32);
typedef _PdDrawerCalibrateNative = Int32 Function(
    Pointer<PdDriver>, Pointer<PdPrinter>, Int32);
typedef _PdDrawerFlagNative = Int32 Function(
    Pointer<PdDriver>, Pointer<PdPrinter>);
typedef _PdTestDrawerCountNative = Size Function(Pointer<PdPrinter>);
typedef _PdTestDrawerFlagNative = Int Function(Pointer<PdPrinter>);
typedef _PdTestSetDrawerOpenNative = Void Function(Pointer<PdPrinter>, Int);

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
typedef _PdTestLinkCreateNative = Pointer<PdTestLink> Function(Pointer<Char>);
typedef _PdTestLinkVoidNative = Void Function(Pointer<PdTestLink>);
typedef _PdTestLinkBindNative = Void Function(
    Pointer<PdTestLink>, Pointer<PdPrinter>);
typedef _PdTestLinkCountNative = Size Function(Pointer<PdTestLink>);
typedef _PdTestLinkContainsNative = Int Function(
    Pointer<PdTestLink>, Pointer<Char>);
typedef _PdTestCppEnumCountNative = Int Function(Int32);
typedef _PdTestCppEnumNameNative = Pointer<Char> Function(Int32, Int);
typedef _PdTestCppEnumValueNative = Int Function(Int32, Int);
typedef _PdTestEnumLabelNative = Pointer<Char> Function(Int32);

// --- M14: cash drawer (docs/cash-drawer.md) ---------------------------------------
//
// Flat by design: no strings anywhere in these, so there is no pointer alignment to get
// wrong on one platform and right on another. Sizes are pinned in
// test/abi_layout_test.dart.

/// `pd_drawer_capabilities` — the drawer facet of a printer's capability profile.
final class PdDrawerCapabilities extends Struct {
  @Int32()
  external int present;

  /// `pd_drawer_port_standard`. Nothing is ever fired on the unknown member.
  @Int32()
  external int standard;

  /// Volts, or 0 where the manufacturer does not document it — not the same as "low".
  @Uint16()
  external int voltage;

  @Uint16()
  external int maxCurrentMa;

  @Uint8()
  external int channelCount;

  /// 3 on the Epson arrangement, 6 on Star's; 0 when unestablished.
  @Uint8()
  external int sensorPin;

  /// `pd_drawer_kick_method`.
  @Int32()
  external int method;

  @Uint16()
  external int defaultPulseMs;

  @Uint16()
  external int maxPulseMs;

  @Uint16()
  external int cooldownMs;

  @Int32()
  external int canKickDuringPrint;

  @Int32()
  external int statusAvailable;

  /// `pd_drawer_status_method`.
  @Int32()
  external int statusMethod;

  /// Two drive outputs, one switch input.
  @Int32()
  external int sharedBetweenDrawers;

  @Int32()
  external int sharedWithBuzzer;

  /// Two columns, never one flag: a documented 24 V port can sit beside an unproven
  /// pulse command, and the XP-S260M is exactly that case.
  @Int32()
  external int electricalProvenance;

  @Int32()
  external int commandsProvenance;

  /// Whether this engine may put a pulse on the wire at all.
  @Int32()
  external int kickable;
}

/// `pd_drawer_request` — 0 in either field asks for the profile's own default.
final class PdDrawerRequest extends Struct {
  @Uint8()
  external int channel;

  @Uint16()
  external int pulseMs;
}

/// `pd_drawer_result` — a state and never a boolean.
final class PdDrawerResult extends Struct {
  @Int32()
  external int state;

  @Int32()
  external int previousState;

  @Uint8()
  external int channel;

  /// 0 when no pulse was emitted: already open, or refused.
  @Uint16()
  external int pulseMs;

  /// Pulse to verdict — the "sensor changed 143 ms after kick" number.
  @Uint32()
  external int elapsedMs;
}

/// `pd_drawer_reading` — one non-destructive read of the drawer switch.
final class PdDrawerReading extends Struct {
  @Int32()
  external int available;

  @Int32()
  external int answered;

  /// Tri-state: `PD_UNKNOWN`, `PD_FALSE` or `PD_TRUE`.
  @Int32()
  external int pinHigh;

  @Int32()
  external int needsCalibration;

  @Int32()
  external int state;
}

// --- M15: self-test, auto-detection and LAN discovery (docs/api.md §15) -------------

/// `pd_detection_summary` — the detection report, shared by the self-test and by
/// auto-detection so the paper, the CLI, the agent and this wrapper describe a device the
/// same way.
final class PdDetectionSummary extends Struct {
  external Pointer<Char> endpoint;

  external Pointer<Char> vendor;
  external Pointer<Char> model;
  external Pointer<Char> firmware;
  external Pointer<Char> serial;

  /// 1 only when a signal independent of `GS I` agrees with `GS I`.
  @Int32()
  external int identityTrusted;

  @Uint8()
  external int confidencePercent;

  @Int32()
  external int impersonationSuspected;

  @Int32()
  external int identityFresh;

  external Pointer<Char> profileId;

  @Int32()
  external int selection;

  @Uint16()
  external int nominalPaperMm;

  @Uint32()
  external int printableWidthDots;

  @Uint32()
  external int charsPerLine;

  @Uint16()
  external int dpi;

  @Int32()
  external int completion;

  @Int32()
  external int gradeCeiling;

  @Int32()
  external int authority;

  external Pointer<Char> method;

  @Int32()
  external int completionProvenance;

  @Int32()
  external int drawerPresent;

  @Int32()
  external int drawerKickable;

  @Int32()
  external int drawerStandard;

  @Uint16()
  external int drawerVoltage;

  @Int32()
  external int drawerElectricalProvenance;

  @Int32()
  external int drawerCommandsProvenance;

  external Pointer<Char> provenanceSummary;

  /// Declared degradations, `degradationCount` entries; `nullptr` when there are none.
  external Pointer<Pointer<Char>> degradations;

  @Size()
  external int degradationCount;
}

/// `pd_self_test_options` — all-zeroes is the useful call.
final class PdSelfTestOptions extends Struct {
  external Pointer<Char> key;

  @Int32()
  external int refreshIdentity;

  @Int32()
  external int probeWithoutPrinting;

  @Int32()
  external int noBarcode;

  external Pointer<Char> barcodeData;

  @Int32()
  external int noVerificationId;

  @Uint32()
  external int timeoutMs;
}

/// `pd_self_test_result` — the ordinary tri-state job result plus what the ticket said.
final class PdSelfTestResult extends Struct {
  external PdJobResult result;
  external PdDetectionSummary detection;
  external Pointer<Char> key;
  external Pointer<Char> printToken;
  external Pointer<Char> ticketText;
  external Pointer<PdJob> job;
}

/// `pd_auto_detect_options`.
final class PdAutoDetectOptions extends Struct {
  external Pointer<Char> subnetCidr;
  external Pointer<Pointer<Char>> endpoints;

  @Uint16()
  external int port;

  @Uint32()
  external int concurrency;

  @Uint32()
  external int connectTimeoutMs;

  @Uint32()
  external int responseTimeoutMs;

  @Int32()
  external int leaveUnknownUnprobed;

  @Uint32()
  external int statusTimeoutMs;

  @Uint32()
  external int identityTimeoutMs;

  @Uint32()
  external int completionTimeoutMs;
}

/// `pd_detected_printer` — one candidate, classified.
final class PdDetectedPrinter extends Struct {
  external Pointer<Char> endpoint;
  external Pointer<Char> host;

  @Uint16()
  external int port;

  @Int32()
  external int status;

  @Int32()
  external int portOpen;

  @Int32()
  external int fromCache;

  external Pointer<Char> dleEotHex;
  external PdDetectionSummary summary;
}

/// `pd_discover_options`.
final class PdDiscoverOptions extends Struct {
  external Pointer<Char> subnetCidr;

  @Uint16()
  external int port;

  @Uint32()
  external int concurrency;

  @Uint32()
  external int connectTimeoutMs;

  @Uint32()
  external int responseTimeoutMs;

  @Int32()
  external int noBackchannelProbe;
}

/// `pd_discovered_device` — one address the sweep found listening.
final class PdDiscoveredDevice extends Struct {
  external Pointer<Char> ip;

  @Uint16()
  external int port;

  @Int32()
  external int port9100Open;

  external Pointer<Char> dleEotHex;
}

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
  binarization(12),
  confidenceGrade(13),
  completionAuthority(14),
  // M14 — docs/cash-drawer.md.
  drawerState(15),
  drawerPortStandard(16),
  drawerKickMethod(17),
  drawerStatusMethod(18),
  // M15 — docs/api.md §15.
  profileSelection(19),
  detectionStatus(20);

  const PdTestEnum(this.nativeValue);

  final int nativeValue;

  /// `PD_TEST_ENUM_TOTAL`: the sentinel, not a member.
  static const int total = 21;
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
        addPrinterCustom = library.lookupFunction<
            _PdAddPrinterCustomNative,
            Pointer<PdPrinter> Function(Pointer<PdDriver>,
                Pointer<PdTransportVtable>, Pointer<Void>, Pointer<Char>, int)>(
          'pd_add_printer_custom',
        ),
        transportFeedBytes = library.lookupFunction<_PdTransportFeedBytesNative,
            int Function(Pointer<PdPrinter>, Pointer<Uint8>, int)>(
          'pd_transport_feed_bytes',
        ),
        transportLinkDropped = library.lookupFunction<
            _PdTransportLinkDroppedNative,
            int Function(Pointer<PdPrinter>, Pointer<Char>)>(
          'pd_transport_link_dropped',
        ),
        printerId = library.lookupFunction<_PdPrinterIdNative,
            Pointer<Char> Function(Pointer<PdPrinter>)>('pd_printer_id'),
        printerWidthDots = library.lookupFunction<_PdPrinterWidthDotsNative,
            int Function(Pointer<PdPrinter>)>('pd_printer_width_dots'),
        printerCompletion = library.lookupFunction<_PdPrinterCompletionNative,
            int Function(Pointer<PdPrinter>)>('pd_printer_completion'),
        printerCompletionProvenance = library.lookupFunction<
            _PdPrinterProvenanceNative,
            int Function(
                Pointer<PdPrinter>)>('pd_printer_completion_provenance'),
        printerLanguage = library.lookupFunction<_PdPrinterLanguageNative,
            int Function(Pointer<PdPrinter>)>('pd_printer_language'),
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
        forceReprintOpts = library.lookupFunction<
            _PdForceReprintOptsNative,
            Pointer<PdJob> Function(
                Pointer<PdDriver>,
                Pointer<PdPrinter>,
                Pointer<Char>,
                Pointer<PdReprintOptions>)>('pd_force_reprint_opts'),
        findJob = library.lookupFunction<
            _PdFindJobNative,
            Pointer<PdJob> Function(
                Pointer<PdDriver>, Pointer<Char>)>('pd_find_job'),
        jobByToken = library.lookupFunction<
            _PdJobByTokenNative,
            Pointer<PdJob> Function(
                Pointer<PdDriver>, Pointer<Char>)>('pd_job_by_token'),
        instanceNonce = library.lookupFunction<_PdInstanceNonceNative,
            Pointer<Char> Function(Pointer<PdDriver>)>('pd_instance_nonce'),
        jobPrintToken = library.lookupFunction<_PdJobIdNative,
            Pointer<Char> Function(Pointer<PdJob>)>('pd_job_print_token'),
        jobCutToken = library.lookupFunction<_PdJobIdNative,
            Pointer<Char> Function(Pointer<PdJob>)>('pd_job_cut_token'),
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
        confidenceGradeName = library.lookupFunction<_PdEnumNameNative,
            Pointer<Char> Function(int)>('pd_confidence_grade_name'),
        completionAuthorityName = library.lookupFunction<_PdEnumNameNative,
            Pointer<Char> Function(int)>('pd_completion_authority_name'),
        provenanceName = library.lookupFunction<_PdEnumNameNative,
            Pointer<Char> Function(int)>('pd_provenance_name'),
        commandLanguageName = library.lookupFunction<_PdEnumNameNative,
            Pointer<Char> Function(int)>('pd_command_language_name'),
        confidenceGradeLetter = library.lookupFunction<_PdEnumNameNative,
            Pointer<Char> Function(int)>('pd_confidence_grade_letter'),
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
  final Pointer<PdPrinter> Function(
      Pointer<PdDriver>,
      Pointer<PdTransportVtable>,
      Pointer<Void>,
      Pointer<Char>,
      int) addPrinterCustom;
  final int Function(Pointer<PdPrinter>, Pointer<Uint8>, int)
      transportFeedBytes;
  final int Function(Pointer<PdPrinter>, Pointer<Char>) transportLinkDropped;
  final Pointer<Char> Function(Pointer<PdPrinter>) printerId;
  final int Function(Pointer<PdPrinter>) printerWidthDots;
  final int Function(Pointer<PdPrinter>) printerCompletion;
  final int Function(Pointer<PdPrinter>) printerCompletionProvenance;
  final int Function(Pointer<PdPrinter>) printerLanguage;
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
  final Pointer<PdJob> Function(Pointer<PdDriver>, Pointer<PdPrinter>,
      Pointer<Char>, Pointer<PdReprintOptions>) forceReprintOpts;
  final Pointer<PdJob> Function(Pointer<PdDriver>, Pointer<Char>) findJob;
  final Pointer<PdJob> Function(Pointer<PdDriver>, Pointer<Char>) jobByToken;
  final Pointer<Char> Function(Pointer<PdDriver>) instanceNonce;
  final Pointer<Char> Function(Pointer<PdJob>) jobPrintToken;
  final Pointer<Char> Function(Pointer<PdJob>) jobCutToken;
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
  final Pointer<Char> Function(int) confidenceGradeName;
  final Pointer<Char> Function(int) completionAuthorityName;
  final Pointer<Char> Function(int) provenanceName;
  final Pointer<Char> Function(int) commandLanguageName;
  final Pointer<Char> Function(int) confidenceGradeLetter;
  final Pointer<Char> Function(int) completionMechanismName;
  final Pointer<Char> Function(int) cutVariantName;
  final int Function(int) codePageAt;

  // --- M13b: the print-queue addon (docs/sdk-spec.md §12) ---------------------------
  //
  // Resolved lazily rather than in the constructor, so that a host binding an older
  // shared library still gets a working driver and only fails if it actually asks for a
  // queue — the same treatment the test-support symbols get below.

  late final Pointer<PdQueue> Function(Pointer<PdDriver>, Pointer<PdQueuePolicy>)
      queueCreate = _library.lookupFunction<
          Pointer<PdQueue> Function(Pointer<PdDriver>, Pointer<PdQueuePolicy>),
          Pointer<PdQueue> Function(
              Pointer<PdDriver>, Pointer<PdQueuePolicy>)>('pd_queue_create');

  late final void Function(Pointer<PdQueue>) queueDestroy =
      _library.lookupFunction<Void Function(Pointer<PdQueue>),
          void Function(Pointer<PdQueue>)>('pd_queue_destroy');

  late final Pointer<PdJob> Function(Pointer<PdQueue>, Pointer<PdPrinter>,
      Pointer<PdPayload>, Pointer<PdQueueOptions>) queueEnqueue =
      _library.lookupFunction<
          Pointer<PdJob> Function(Pointer<PdQueue>, Pointer<PdPrinter>,
              Pointer<PdPayload>, Pointer<PdQueueOptions>),
          Pointer<PdJob> Function(Pointer<PdQueue>, Pointer<PdPrinter>,
              Pointer<PdPayload>, Pointer<PdQueueOptions>)>('pd_queue_enqueue');

  late final void Function(Pointer<PdQueue>, Pointer<Char>) queuePause =
      _library.lookupFunction<Void Function(Pointer<PdQueue>, Pointer<Char>),
          void Function(Pointer<PdQueue>, Pointer<Char>)>('pd_queue_pause');

  late final void Function(Pointer<PdQueue>, Pointer<Char>) queueResume =
      _library.lookupFunction<Void Function(Pointer<PdQueue>, Pointer<Char>),
          void Function(Pointer<PdQueue>, Pointer<Char>)>('pd_queue_resume');

  late final int Function(Pointer<PdQueue>, Pointer<Char>) queueIsPaused =
      _library.lookupFunction<Int32 Function(Pointer<PdQueue>, Pointer<Char>),
          int Function(Pointer<PdQueue>, Pointer<Char>)>('pd_queue_is_paused');

  late final int Function(Pointer<PdQueue>, Pointer<Char>) queueIsBlocked =
      _library.lookupFunction<Int32 Function(Pointer<PdQueue>, Pointer<Char>),
          int Function(Pointer<PdQueue>, Pointer<Char>)>('pd_queue_is_blocked');

  late final void Function(Pointer<PdQueue>, Pointer<Char>) queueUnblock =
      _library.lookupFunction<Void Function(Pointer<PdQueue>, Pointer<Char>),
          void Function(Pointer<PdQueue>, Pointer<Char>)>('pd_queue_unblock');

  late final int Function(Pointer<PdQueue>, Pointer<Char>) queuePending =
      _library.lookupFunction<Size Function(Pointer<PdQueue>, Pointer<Char>),
          int Function(Pointer<PdQueue>, Pointer<Char>)>('pd_queue_pending');

  late final int Function(Pointer<PdQueue>) queueExpiredCount =
      _library.lookupFunction<Size Function(Pointer<PdQueue>),
          int Function(Pointer<PdQueue>)>('pd_queue_expired_count');

  late final int Function(Pointer<PdQueue>) queueOverflowCount =
      _library.lookupFunction<Size Function(Pointer<PdQueue>),
          int Function(Pointer<PdQueue>)>('pd_queue_overflow_count');

  late final int Function(Pointer<PdQueue>) queueDrainedCount =
      _library.lookupFunction<Size Function(Pointer<PdQueue>),
          int Function(Pointer<PdQueue>)>('pd_queue_drained_count');

  late final void Function(Pointer<PdQueue>) queueTick = _library.lookupFunction<
      Void Function(Pointer<PdQueue>), void Function(Pointer<PdQueue>)>('pd_queue_tick');

  // --- M14: cash drawer (docs/cash-drawer.md) --------------------------------------
  //
  // Resolved lazily rather than in the constructor above, so that adding the drawer
  // surface did not reorder a single existing lookup. They are ordinary shipped
  // symbols, unlike the test-support block further down.

  /// `pd_printer_drawer_capabilities`
  late final PdDrawerCapabilities Function(Pointer<PdPrinter>)
      printerDrawerCapabilities = _library.lookupFunction<
          _PdPrinterDrawerCapabilitiesNative,
          PdDrawerCapabilities Function(Pointer<PdPrinter>)>(
    'pd_printer_drawer_capabilities',
  );

  /// `pd_drawer_open` — the verified opening sequence.
  late final PdDrawerResult Function(
          Pointer<PdDriver>, Pointer<PdPrinter>, Pointer<PdDrawerRequest>)
      drawerOpen = _library.lookupFunction<
          _PdDrawerOpenNative,
          PdDrawerResult Function(Pointer<PdDriver>, Pointer<PdPrinter>,
              Pointer<PdDrawerRequest>)>('pd_drawer_open');

  /// `pd_drawer_read_sensor` — never pulses.
  late final PdDrawerReading Function(Pointer<PdDriver>, Pointer<PdPrinter>, int)
      drawerReadSensor = _library.lookupFunction<
          _PdDrawerReadSensorNative,
          PdDrawerReading Function(
              Pointer<PdDriver>, Pointer<PdPrinter>, int)>('pd_drawer_read_sensor');

  /// `pd_drawer_calibrate_polarity`
  late final int Function(Pointer<PdDriver>, Pointer<PdPrinter>, int)
      drawerCalibratePolarity = _library.lookupFunction<
          _PdDrawerCalibrateNative,
          int Function(Pointer<PdDriver>, Pointer<PdPrinter>,
              int)>('pd_drawer_calibrate_polarity');

  /// `pd_drawer_polarity_calibrated`
  late final int Function(Pointer<PdDriver>, Pointer<PdPrinter>)
      drawerPolarityCalibrated = _library.lookupFunction<_PdDrawerFlagNative,
          int Function(Pointer<PdDriver>, Pointer<PdPrinter>)>(
    'pd_drawer_polarity_calibrated',
  );

  /// `pd_drawer_high_means_open`
  late final int Function(Pointer<PdDriver>, Pointer<PdPrinter>)
      drawerHighMeansOpen = _library.lookupFunction<_PdDrawerFlagNative,
          int Function(Pointer<PdDriver>, Pointer<PdPrinter>)>(
    'pd_drawer_high_means_open',
  );

  /// `pd_drawer_state_name`
  late final Pointer<Char> Function(int) drawerStateName =
      _library.lookupFunction<_PdEnumNameNative, Pointer<Char> Function(int)>(
    'pd_drawer_state_name',
  );

  /// `pd_drawer_port_standard_name`
  late final Pointer<Char> Function(int) drawerPortStandardName =
      _library.lookupFunction<_PdEnumNameNative, Pointer<Char> Function(int)>(
    'pd_drawer_port_standard_name',
  );

  /// `pd_drawer_kick_method_name`
  late final Pointer<Char> Function(int) drawerKickMethodName =
      _library.lookupFunction<_PdEnumNameNative, Pointer<Char> Function(int)>(
    'pd_drawer_kick_method_name',
  );

  /// `pd_drawer_status_method_name`
  late final Pointer<Char> Function(int) drawerStatusMethodName =
      _library.lookupFunction<_PdEnumNameNative, Pointer<Char> Function(int)>(
    'pd_drawer_status_method_name',
  );

  // --- M15: self-test, auto-detection and LAN discovery (docs/api.md §15) ------------
  //
  // Lazy, like the two blocks above, so adding them reorders no existing lookup.
  //
  // Note what is NOT bound here: the per-candidate callbacks pd_auto_detect and
  // pd_discover accept. A Dart isolate blocked inside an FFI call cannot service a
  // native callback until that call returns, so this wrapper passes NULL and reads the
  // results back with pd_detected_at / pd_discovered_at, which pd.h provides for exactly
  // this reason.

  /// `pd_self_test` — prints one diagnostic ticket. Uses paper.
  late final int Function(Pointer<PdDriver>, Pointer<PdPrinter>,
      Pointer<PdSelfTestOptions>, Pointer<PdSelfTestResult>) selfTest =
      _library.lookupFunction<
          Int32 Function(Pointer<PdDriver>, Pointer<PdPrinter>,
              Pointer<PdSelfTestOptions>, Pointer<PdSelfTestResult>),
          int Function(Pointer<PdDriver>, Pointer<PdPrinter>,
              Pointer<PdSelfTestOptions>, Pointer<PdSelfTestResult>)>('pd_self_test');

  /// `pd_auto_detect` — nothing prints and nothing fires.
  late final int Function(
          Pointer<PdDriver>, Pointer<PdAutoDetectOptions>, Pointer<Void>, Pointer<Void>)
      autoDetect = _library.lookupFunction<
          Int32 Function(Pointer<PdDriver>, Pointer<PdAutoDetectOptions>, Pointer<Void>,
              Pointer<Void>),
          int Function(Pointer<PdDriver>, Pointer<PdAutoDetectOptions>, Pointer<Void>,
              Pointer<Void>)>('pd_auto_detect');

  /// `pd_discover` — the raw sweep, DLE EOT 1 and nothing else.
  late final int Function(
          Pointer<PdDriver>, Pointer<PdDiscoverOptions>, Pointer<Void>, Pointer<Void>)
      discover = _library.lookupFunction<
          Int32 Function(Pointer<PdDriver>, Pointer<PdDiscoverOptions>, Pointer<Void>,
              Pointer<Void>),
          int Function(Pointer<PdDriver>, Pointer<PdDiscoverOptions>, Pointer<Void>,
              Pointer<Void>)>('pd_discover');

  /// `pd_detected_at`
  late final int Function(Pointer<PdDriver>, int, Pointer<PdDetectedPrinter>)
      detectedAt = _library.lookupFunction<
          Int32 Function(Pointer<PdDriver>, Int32, Pointer<PdDetectedPrinter>),
          int Function(Pointer<PdDriver>, int,
              Pointer<PdDetectedPrinter>)>('pd_detected_at');

  /// `pd_discovered_at`
  late final int Function(Pointer<PdDriver>, int, Pointer<PdDiscoveredDevice>)
      discoveredAt = _library.lookupFunction<
          Int32 Function(Pointer<PdDriver>, Int32, Pointer<PdDiscoveredDevice>),
          int Function(Pointer<PdDriver>, int,
              Pointer<PdDiscoveredDevice>)>('pd_discovered_at');

  /// `pd_local_subnet`
  late final Pointer<Char> Function(Pointer<PdDriver>) localSubnet =
      _library.lookupFunction<Pointer<Char> Function(Pointer<PdDriver>),
          Pointer<Char> Function(Pointer<PdDriver>)>('pd_local_subnet');

  /// `pd_profile_selection_name`
  late final Pointer<Char> Function(int) profileSelectionName =
      _library.lookupFunction<_PdEnumNameNative, Pointer<Char> Function(int)>(
    'pd_profile_selection_name',
  );

  /// `pd_detection_status_name`
  late final Pointer<Char> Function(int) detectionStatusName =
      _library.lookupFunction<_PdEnumNameNative, Pointer<Char> Function(int)>(
    'pd_detection_status_name',
  );

  /// `pd_test_drawer_kicks` — pulses that reached the scripted device. Test-only.
  late final int Function(Pointer<PdPrinter>) testDrawerKicks =
      _library.lookupFunction<_PdTestDrawerCountNative,
          int Function(Pointer<PdPrinter>)>('pd_test_drawer_kicks');

  /// `pd_test_drawer_is_open` — where the scripted microswitch sits. Test-only.
  late final int Function(Pointer<PdPrinter>) testDrawerIsOpen =
      _library.lookupFunction<_PdTestDrawerFlagNative,
          int Function(Pointer<PdPrinter>)>('pd_test_drawer_is_open');

  /// `pd_test_set_drawer_open` — an operator's hand on the drawer. Test-only.
  late final void Function(Pointer<PdPrinter>, int) testSetDrawerOpen =
      _library.lookupFunction<_PdTestSetDrawerOpenNative,
          void Function(Pointer<PdPrinter>, int)>('pd_test_set_drawer_open');

  // --- M16: custom method registration (docs/api.md §16) ----------------------------
  //
  // Lazy, like the blocks above. Every callback in this group is invoked on a CORE thread
  // and has to answer there and then, which `dart:ffi` cannot do from Dart code -- see the
  // header comment of custom_methods.dart for the three callback kinds and why none of
  // them fits. So these take native function pointers, exactly as the custom-transport
  // vtable does, and the structs below are transcriptions of pd.h and nothing more.

  /// `pd_register_completion_method`
  late final int Function(Pointer<PdDriver>, Pointer<PdCompletionMethod>)
      registerCompletionMethod = _library.lookupFunction<
          Int32 Function(Pointer<PdDriver>, Pointer<PdCompletionMethod>),
          int Function(Pointer<PdDriver>,
              Pointer<PdCompletionMethod>)>('pd_register_completion_method');

  /// `pd_register_probe_step`
  late final int Function(Pointer<PdDriver>, Pointer<PdProbeStep>) registerProbeStep =
      _library.lookupFunction<
          Int32 Function(Pointer<PdDriver>, Pointer<PdProbeStep>),
          int Function(
              Pointer<PdDriver>, Pointer<PdProbeStep>)>('pd_register_probe_step');

  /// `pd_register_block_handler`
  late final int Function(Pointer<PdDriver>, Pointer<PdBlockHandler>)
      registerBlockHandler = _library.lookupFunction<
          Int32 Function(Pointer<PdDriver>, Pointer<PdBlockHandler>),
          int Function(Pointer<PdDriver>,
              Pointer<PdBlockHandler>)>('pd_register_block_handler');

  /// `pd_register_formatter`
  late final int Function(Pointer<PdDriver>, Pointer<PdFormatter>) registerFormatter =
      _library.lookupFunction<
          Int32 Function(Pointer<PdDriver>, Pointer<PdFormatter>),
          int Function(
              Pointer<PdDriver>, Pointer<PdFormatter>)>('pd_register_formatter');

  /// `pd_register_drawer_kick`
  late final int Function(Pointer<PdDriver>, Pointer<PdDrawerKickReg>)
      registerDrawerKick = _library.lookupFunction<
          Int32 Function(Pointer<PdDriver>, Pointer<PdDrawerKickReg>),
          int Function(Pointer<PdDriver>,
              Pointer<PdDrawerKickReg>)>('pd_register_drawer_kick');

  /// `pd_match_kind_name`
  late final Pointer<Char> Function(int) matchKindName =
      _library.lookupFunction<_PdEnumNameNative, Pointer<Char> Function(int)>(
    'pd_match_kind_name',
  );

  /// `pd_drain_order_name`
  late final Pointer<Char> Function(int) drainOrderName =
      _library.lookupFunction<_PdEnumNameNative, Pointer<Char> Function(int)>(
    'pd_drain_order_name',
  );

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

  /// `pd_test_link_create` — script ids `ok` and `gsr1`; null for an unknown one.
  ///
  /// The link owns the thread that answers, which is the whole reason it exists: pd.h
  /// forbids feeding bytes from inside `write`, so a far side that responds has to
  /// deliver them from somewhere else, exactly as a CoreBluetooth delegate queue or an
  /// Android reader thread does.
  late final Pointer<PdTestLink> Function(Pointer<Char>) testLinkCreate =
      _library.lookupFunction<_PdTestLinkCreateNative,
          Pointer<PdTestLink> Function(Pointer<Char>)>('pd_test_link_create');

  /// `pd_test_link_destroy` — after `pd_destroy`, never before.
  late final void Function(Pointer<PdTestLink>) testLinkDestroy =
      _library.lookupFunction<_PdTestLinkVoidNative,
          void Function(Pointer<PdTestLink>)>('pd_test_link_destroy');

  /// `pd_test_link_bind` — tells the reader thread which printer to feed.
  late final void Function(Pointer<PdTestLink>, Pointer<PdPrinter>)
      testLinkBind = _library.lookupFunction<_PdTestLinkBindNative,
          void Function(Pointer<PdTestLink>, Pointer<PdPrinter>)>(
    'pd_test_link_bind',
  );

  /// `pd_test_link_refuse_connections`
  late final void Function(Pointer<PdTestLink>) testLinkRefuseConnections =
      _library.lookupFunction<_PdTestLinkVoidNative,
          void Function(Pointer<PdTestLink>)>(
    'pd_test_link_refuse_connections',
  );

  /// `pd_test_link_connects`
  late final int Function(Pointer<PdTestLink>) testLinkConnects =
      _library.lookupFunction<_PdTestLinkCountNative,
          int Function(Pointer<PdTestLink>)>('pd_test_link_connects');

  /// `pd_test_link_closes`
  late final int Function(Pointer<PdTestLink>) testLinkCloses =
      _library.lookupFunction<_PdTestLinkCountNative,
          int Function(Pointer<PdTestLink>)>('pd_test_link_closes');

  /// `pd_test_link_bytes_written`
  late final int Function(Pointer<PdTestLink>) testLinkBytesWritten =
      _library.lookupFunction<_PdTestLinkCountNative,
          int Function(Pointer<PdTestLink>)>('pd_test_link_bytes_written');

  /// `pd_test_link_cuts`
  late final int Function(Pointer<PdTestLink>) testLinkCuts =
      _library.lookupFunction<_PdTestLinkCountNative,
          int Function(Pointer<PdTestLink>)>('pd_test_link_cuts');

  /// `pd_test_link_received_contains`
  late final int Function(Pointer<PdTestLink>, Pointer<Char>)
      testLinkReceivedContains = _library.lookupFunction<
          _PdTestLinkContainsNative,
          int Function(Pointer<PdTestLink>,
              Pointer<Char>)>('pd_test_link_received_contains');

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

// --- M16: custom method registration (docs/api.md §16) --------------------------------

/// `pd_match_result` — a custom matcher's verdict plus the token it matched.
final class PdMatchResult extends Struct {
  @Int32()
  external int kind;

  /// NUL-terminated; the core copies it out before the matcher returns.
  @Array(8)
  external Array<Uint8> token;
}

/// `pd_probe_finding` — what one custom probe step concluded.
final class PdProbeFinding extends Struct {
  @Int32()
  external int answered;

  @Array(64)
  external Array<Uint8> label;
}

/// `pd_fence_bytes_fn`
typedef PdFenceBytesNative = Size Function(
    Pointer<Void>, Pointer<Char>, Pointer<Uint8>, Size);

/// `pd_completion_matcher_fn`
typedef PdCompletionMatcherNative = PdMatchResult Function(
    Pointer<Void>, Pointer<Uint8>, Size);

/// `pd_probe_classify_fn`
typedef PdProbeClassifyNative = PdProbeFinding Function(
    Pointer<Void>, Pointer<Uint8>, Size);

/// `pd_block_handler_fn`
typedef PdBlockHandlerNative = Size Function(Pointer<Void>, Pointer<Char>, Pointer<Char>,
    Pointer<Uint8>, Size, Pointer<Int32>, Pointer<Char>, Size);

/// `pd_formatter_fn`
typedef PdFormatterNative = Size Function(Pointer<Void>, Pointer<Char>, Pointer<Char>,
    Pointer<Char>, Pointer<Char>, Size, Pointer<Int32>);

/// `pd_drawer_kick_bytes_fn`
typedef PdDrawerKickBytesNative = Size Function(
    Pointer<Void>, Uint8, Uint16, Pointer<Uint8>, Size);

/// `pd_drawer_status_request_fn`
typedef PdDrawerStatusRequestNative = Size Function(
    Pointer<Void>, Pointer<Uint8>, Size);

/// `pd_drawer_status_parse_fn`
typedef PdDrawerStatusParseNative = Int32 Function(
    Pointer<Void>, Pointer<Uint8>, Size);

/// `pd_completion_method`
final class PdCompletionMethod extends Struct {
  external Pointer<Char> id;
  external Pointer<NativeFunction<PdFenceBytesNative>> fenceBytes;
  external Pointer<NativeFunction<PdCompletionMatcherNative>> matcher;
  external Pointer<Void> ctx;

  @Int32()
  external int grade;

  @Int32()
  external int authority;

  external Pointer<Char> methodName;
}

/// `pd_probe_step`
final class PdProbeStep extends Struct {
  external Pointer<Char> id;
  external Pointer<Uint8> requestBytes;

  @Size()
  external int requestSize;

  external Pointer<NativeFunction<PdProbeClassifyNative>> classify;
  external Pointer<Void> ctx;
}

/// `pd_block_handler`
final class PdBlockHandler extends Struct {
  external Pointer<Char> kind;
  external Pointer<NativeFunction<PdBlockHandlerNative>> handler;
  external Pointer<Void> ctx;
}

/// `pd_formatter`
final class PdFormatter extends Struct {
  external Pointer<Char> name;
  external Pointer<NativeFunction<PdFormatterNative>> formatter;
  external Pointer<Void> ctx;
}

/// `pd_drawer_kick_reg`
final class PdDrawerKickReg extends Struct {
  external Pointer<Char> id;
  external Pointer<NativeFunction<PdDrawerKickBytesNative>> kickBytes;

  /// Optional, and they go together: both `nullptr` means the vendor method has no
  /// readable switch, so a kick reports KICK_SENT_UNVERIFIED rather than a verified open.
  external Pointer<NativeFunction<PdDrawerStatusRequestNative>> statusRequest;
  external Pointer<NativeFunction<PdDrawerStatusParseNative>> statusParse;

  external Pointer<Void> ctx;
}
