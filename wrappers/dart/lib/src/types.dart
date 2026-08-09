import 'dart:ffi';
import 'dart:typed_data';

import 'allocation.dart';
import 'bindings.dart';
import 'enums.dart';

/// One step of a job's history, exactly as the core recorded it.
final class JobEvent {
  const JobEvent({
    required this.state,
    required this.confidence,
    required this.reason,
    required this.monotonicMs,
  });

  /// Reads a `pd_job_event` the ABI handed over by value.
  ///
  /// By value is what makes this safe from a `NativeCallable.listener`, which runs
  /// after the native call has already returned: there is no frame left to point into.
  factory JobEvent.fromNative(PdJobEvent event) => JobEvent(
        state: JobState.fromNative(event.state),
        confidence: ConfidenceLevel.fromNative(event.confidence),
        reason: event.hasReason != 0
            ? FailureReason.fromNative(event.reason)
            : null,
        monotonicMs: event.monotonicMs,
      );

  final JobState state;

  /// What the claim rests on at this point. Never inflated.
  final ConfidenceLevel confidence;

  /// Null for every non-failure transition — the tri-state's honesty starts here.
  final FailureReason? reason;

  /// A steady-clock stamp in milliseconds. Meaningful against other events of the same
  /// process, not against the wall clock, which may step.
  final int monotonicMs;

  @override
  String toString() => 'JobEvent(${state.name}, ${confidence.name}'
      '${reason == null ? '' : ', ${reason!.name}'})';
}

/// The terminal answer for a job: deliberately tri-state (docs/api.md §4).
///
/// There is no success boolean anywhere in this API. Collapsing [JobUnknown] into
/// either bucket is the bug that produces duplicate kitchen tickets, so the type system
/// refuses to let a caller do it by accident: a `switch` over this sealed hierarchy has
/// to name all three cases.
///
/// ```dart
/// switch (await job.result) {
///   case JobDone(:final confidence): markPrinted(confidence);
///   case JobFailed(:final reason):   showFailure(reason);   // safe to resubmit the key
///   case JobUnknown():               askOperator(job);      // forceReprint or confirm
/// }
/// ```
sealed class JobResult {
  const JobResult({
    required this.confidence,
    required this.grade,
    required this.authority,
    required this.method,
  });

  /// How far up the evidence ladder the job got.
  ///
  /// Carried on all three outcomes, not only on [JobDone]: on a failure or an unknown
  /// it is what an operator needs in order to decide about a reprint.
  final ConfidenceLevel confidence;

  /// The class of evidence behind the claim (docs/api.md §13,
  /// docs/compatibility-brief.md §24).
  ///
  /// The core's own answer, carried by `pd_job_result` on every outcome for the same
  /// reason [confidence] is. Orthogonal to the level: the level says how far up the
  /// ladder the job climbed, the grade says what the claim is made of. A refusal grades
  /// [ConfidenceGrade.eTransportOnly], which is the honest reading of a job that never
  /// reached a device.
  final ConfidenceGrade grade;

  /// Who is making the claim — the printer itself, a spooler, or nobody.
  final CompletionAuthority authority;

  /// The command behind the claim, e.g. `GS(H) fn48`. `none` when nothing was
  /// confirmed: the string a support engineer needs six months later.
  final String method;

  /// The `pd_job_outcome` this result is, for the one place the number is needed:
  /// asking the core for its own spelling of the outcome
  /// ([PrinterDriver.abiName]). Dart models the tri-state as sealed types rather than as
  /// an enum — pattern matching on them is exhaustive, which a wrapper-side enum could
  /// never be — so this is the only bridge back to `pd_job_outcome_name`.
  int get nativeOutcome => switch (this) {
        JobDone() => 0,
        JobFailed() => 1,
        JobUnknown() => 2,
      };
}

/// The job reached `DoneSoftware` (or `PhysicallyVerified`).
final class JobDone extends JobResult {
  const JobDone({
    required super.confidence,
    required super.grade,
    required super.authority,
    required super.method,
  });

  @override
  String toString() =>
      'JobDone(${confidence.name}, ${grade.letter}/${authority.name}, $method)';
}

/// The job failed and the failure is confirmed: nothing printed, or the failure itself
/// was reported.
final class JobFailed extends JobResult {
  const JobFailed({
    required super.confidence,
    required super.grade,
    required super.authority,
    required super.method,
    required this.reason,
  });

  final FailureReason reason;

  @override
  String toString() => 'JobFailed(${reason.name}, reached ${confidence.name})';
}

/// Bytes were sent and never acknowledged: a timeout, a dropped link, a crash.
///
/// Not a success and not a failure. Surface it to an operator; resolve it with
/// `forceReprint` or with a manual confirmation.
final class JobUnknown extends JobResult {
  const JobUnknown({
    required super.confidence,
    required super.grade,
    required super.authority,
    required super.method,
    required this.reason,
  });

  /// Usually [FailureReason.unknown]; [FailureReason.timeoutAwaitingCompletion] when
  /// the completion wait ran out, which is the case worth telling an operator about.
  final FailureReason reason;

  @override
  String toString() => 'JobUnknown(${reason.name}, reached ${confidence.name})';
}

/// Builds the tri-state from `pd_job_result`.
///
/// Throws [UnrecognizedNativeValue] rather than guessing when the outcome is not one of
/// the three this build knows: a fourth outcome cannot be represented, and mapping it
/// onto any of the three would be the exact failure mode the tri-state prevents.
JobResult jobResultFromNative(PdJobResult result) {
  final confidence = ConfidenceLevel.fromNative(result.confidence);
  final reason = FailureReason.fromNative(result.reason);
  final grade = ConfidenceGrade.fromNative(result.grade);
  final authority = CompletionAuthority.fromNative(result.authority);
  // Copied out here: the ABI owns the buffer, and it lives as long as the driver rather
  // than as long as this struct.
  final method = readNativeString(result.method);
  return switch (result.outcome) {
    0 => JobDone(
        confidence: confidence,
        grade: grade,
        authority: authority,
        method: method,
      ),
    1 => JobFailed(
        confidence: confidence,
        grade: grade,
        authority: authority,
        method: method,
        reason: reason,
      ),
    2 => JobUnknown(
        confidence: confidence,
        grade: grade,
        authority: authority,
        method: method,
        reason: reason,
      ),
    _ => throw UnrecognizedNativeValue('pd_job_outcome', result.outcome),
  };
}

/// Last known device state — a snapshot the core already had, never a live query, so
/// reading it cannot block behind a print.
///
/// Every field except [connected] and [observed] is a tri-state: null means the device
/// has not said.
final class DeviceStatus {
  const DeviceStatus({
    required this.connected,
    required this.observed,
    required this.online,
    required this.coverOpen,
    required this.paperOut,
    required this.paperNearEnd,
    required this.cutterError,
    required this.unrecoverableError,
    required this.recoverableError,
  });

  factory DeviceStatus.fromNative(PdDeviceStatus status) => DeviceStatus(
        connected: status.connected != 0,
        observed: status.observed != 0,
        online: _triState(status.online),
        coverOpen: _triState(status.coverOpen),
        paperOut: _triState(status.paperOut),
        paperNearEnd: _triState(status.paperNearEnd),
        cutterError: _triState(status.cutterError),
        unrecoverableError: _triState(status.unrecoverableError),
        recoverableError: _triState(status.recoverableError),
      );

  /// Whether the transport currently holds a connection.
  final bool connected;

  /// False until a status frame has actually been decoded. A snapshot that has never
  /// heard from the device says so rather than reporting healthy.
  final bool observed;

  final bool? online;
  final bool? coverOpen;
  final bool? paperOut;
  final bool? paperNearEnd;
  final bool? cutterError;
  final bool? unrecoverableError;
  final bool? recoverableError;

  @override
  String toString() =>
      'DeviceStatus(connected: $connected, observed: $observed, '
      'online: $online, coverOpen: $coverOpen, paperOut: $paperOut, '
      'paperNearEnd: $paperNearEnd, cutterError: $cutterError, '
      'unrecoverableError: $unrecoverableError, recoverableError: $recoverableError)';
}

bool? _triState(int value) => value < 0 ? null : value != 0;

/// Per-job settings. All optional: the zero value of every field is its documented
/// default, and no default trades durability for speed.
final class JobOptions {
  const JobOptions({
    this.key,
    this.cut = CutSetting.profile,
    this.openDrawer = false,
    this.preflight = PreflightMode.strict,
    this.timeout,
    this.topFeedDots = 0,
    this.bottomFeedDots = 0,
    this.printsVerificationId = true,
  });

  /// The idempotency key: a caller-supplied stable id such as an order or ticket UUID.
  ///
  /// Resubmitting a key that already has a job does not print — it returns that job.
  /// Omitting it means the SDK generates one, and then there is no dedupe protection
  /// across restarts, which is why a POS integration should always pass one.
  final String? key;

  final CutSetting cut;

  final bool openDrawer;

  /// [PreflightMode.strict] refuses the job on cover-open or paper-out before a single
  /// payload byte is written.
  final PreflightMode preflight;

  /// The completion-wait budget. Null means the profile's own timeout.
  final Duration? timeout;

  /// Blank paper fed before the first content line — tear-off clearance, presentation
  /// space (docs/receipt-dsl.md "Margins").
  final int topFeedDots;

  /// The *total* whitespace between the last content and the cut.
  ///
  /// The core feeds `max(the profile's blade clearance, this)`, so this can only ever
  /// add paper: a value below the hardware minimum is ignored rather than allowed to
  /// slice through a trailing QR.
  final int bottomFeedDots;

  /// Print the receipt verification identifier in the ticket trailer — the `ORDER:`
  /// line and the trailer QR (docs/api.md §14). On by default.
  ///
  /// Turning it off suppresses the ink, not the evidence: the token is still journaled
  /// and [PrinterDriver.jobByToken] still resolves it.
  final bool printsVerificationId;

  /// Writes this into a zeroed `pd_job_options`.
  void fillNative(Arena arena, Pointer<PdJobOptions> out) =>
      fillStruct(arena, out.ref);

  /// The same, against a struct the caller already has — a nested
  /// `pd_reprint_options.job`, for instance, where there is no separate pointer.
  void fillStruct(Arena arena, PdJobOptions options) {
    options.key = arena.string(key);
    options.cut = _requireKnown(cut.nativeValue, 'cut', cut.name);
    options.openDrawer = openDrawer ? 1 : 0;
    options.preflight =
        _requireKnown(preflight.nativeValue, 'preflight', preflight.name);
    options.timeoutMs = timeout?.inMilliseconds ?? 0;
    options.topFeedDots = topFeedDots;
    options.bottomFeedDots = bottomFeedDots;
    // Inverted in the ABI so that an all-zeroes struct still prints the evidence.
    options.suppressVerificationId = printsVerificationId ? 0 : 1;
  }
}

/// Everything optional about a deliberate duplicate — a mirror of
/// `pd_reprint_options`.
final class ReprintOptions {
  const ReprintOptions({
    this.job = const JobOptions(),
    this.banner = true,
  });

  /// The submission options the duplicate is printed with.
  final JobOptions job;

  /// Print `*** REPRINT / POSSIBLE DUPLICATE ***` and the attempt counter. On by
  /// default.
  ///
  /// Turning it off is a per-call, deliberate act for a receipt where the banner is
  /// inappropriate — a customer-facing copy. A kitchen ticket should never turn it off:
  /// the banner is what lets staff bin the duplicate instead of cooking it twice.
  final bool banner;

  /// Writes this into a zeroed `pd_reprint_options`.
  void fillNative(Arena arena, Pointer<PdReprintOptions> out) {
    job.fillStruct(arena, out.ref.job);
    out.ref.suppressBanner = banner ? 0 : 1;
  }
}

int _requireKnown(int value, String field, String name) {
  if (value < 0) {
    throw ArgumentError.value(
      name,
      field,
      'is not a value the native library defines; it exists only as the landing spot '
      'for enum members a newer library added',
    );
  }
  return value;
}

/// One operation of the document tier.
///
/// Deliberately minimal: everything a receipt needs and nothing a layout engine would
/// need. Richer documents belong in the raster tier.
final class DocumentOp {
  const DocumentOp._(this._kind, this._text, this._value);

  /// Text with no line break.
  const DocumentOp.text(String text) : this._(0, text, 0);

  /// Text followed by a line feed. A null [text] emits a bare line feed.
  const DocumentOp.line([String? text]) : this._(1, text, 0);

  /// `ESC a n`.
  DocumentOp.align(Alignment alignment)
      : this._(2, null,
            _requireKnown(alignment.nativeValue, 'alignment', alignment.name));

  const DocumentOp.bold(bool enabled) : this._(3, null, enabled ? 1 : 0);

  /// Feeds [lines] lines, 1..255.
  DocumentOp.feed(int lines) : this._(4, null, _checkedFeed(lines));

  final int _kind;
  final String? _text;
  final int _value;
}

int _checkedFeed(int lines) {
  if (lines < 1 || lines > 255) {
    throw RangeError.range(lines, 1, 255, 'lines');
  }
  return lines;
}

/// What to print. Three tiers, exactly as docs/api.md §3 defines them.
sealed class Payload {
  const Payload();

  /// Tier 1 — straight RGBA8, as every platform's bitmap hands it over.
  ///
  /// The core composites alpha over white paper, converts to grey with the ITU-R 601
  /// luma weights, scales to the printer's dot width, binarizes and bands, all in
  /// integer arithmetic, so the same pixels always produce the same bytes.
  /// [strideBytes] is 0 for tightly packed rows.
  const factory Payload.raster(
    Uint8List rgba8, {
    required int width,
    required int height,
    int strideBytes,
    Binarization binarization,
    int threshold,
    int maxRowsPerBand,
  }) = RasterPayload;

  /// Tier 1 from 8-bit grey, one byte per pixel.
  ///
  /// The ABI takes RGBA8 only, so this expands each byte to `(g, g, g, 255)`. That is a
  /// format change and not a decision: with the core's ITU-R 601 weights the grey it
  /// recovers is the byte that went in, exactly.
  factory Payload.grayscale(
    Uint8List gray, {
    required int width,
    required int height,
    Binarization binarization,
    int threshold,
    int maxRowsPerBand,
  }) = RasterPayload.fromGrayscale;

  /// Tier 2 — the document builder, for apps without a renderer.
  const factory Payload.document(List<DocumentOp> ops, {CodePage codePage}) =
      DocumentPayload;

  /// Tier 3 — bytes passed through verbatim.
  ///
  /// Must not embed its own cuts or realtime status tricks: the core owns job
  /// termination and its trailing fence assumes that.
  const factory Payload.raw(Uint8List bytes) = RawPayload;

  PayloadKind get kind;

  /// Writes this into a zeroed `pd_payload`, allocating any buffers from [arena].
  void fillNative(Arena arena, Pointer<PdPayload> out);
}

/// Tier 1 (`PD_PAYLOAD_RASTER_RGBA8`).
final class RasterPayload extends Payload {
  const RasterPayload(
    this.rgba8, {
    required this.width,
    required this.height,
    this.strideBytes = 0,
    this.binarization = Binarization.fixedThreshold,
    this.threshold = 0,
    this.maxRowsPerBand = 0,
  });

  factory RasterPayload.fromGrayscale(
    Uint8List gray, {
    required int width,
    required int height,
    Binarization binarization = Binarization.fixedThreshold,
    int threshold = 0,
    int maxRowsPerBand = 0,
  }) {
    final expected = width * height;
    if (gray.length < expected) {
      throw ArgumentError.value(
        gray.length,
        'gray',
        'holds fewer bytes than width * height ($expected)',
      );
    }
    final rgba8 = Uint8List(expected * 4);
    for (var i = 0; i < expected; i++) {
      final value = gray[i];
      final base = i * 4;
      rgba8[base] = value;
      rgba8[base + 1] = value;
      rgba8[base + 2] = value;
      rgba8[base + 3] = 255;
    }
    return RasterPayload(
      rgba8,
      width: width,
      height: height,
      binarization: binarization,
      threshold: threshold,
      maxRowsPerBand: maxRowsPerBand,
    );
  }

  final Uint8List rgba8;
  final int width;
  final int height;
  final int strideBytes;
  final Binarization binarization;

  /// Only read for [Binarization.fixedThreshold]; 0 means 128.
  final int threshold;

  /// 0 means 1024, the Epson tall-image split.
  final int maxRowsPerBand;

  @override
  PayloadKind get kind => PayloadKind.rasterRgba8;

  @override
  void fillNative(Arena arena, Pointer<PdPayload> out) {
    out.ref.kind = PayloadKind.rasterRgba8.nativeValue;
    final raster = out.ref.as.raster;
    raster.pixels = arena.bytes(rgba8);
    raster.width = width;
    raster.height = height;
    raster.strideBytes = strideBytes;
    raster.binarization = _requireKnown(
        binarization.nativeValue, 'binarization', binarization.name);
    raster.threshold = threshold;
    raster.maxRowsPerBand = maxRowsPerBand;
  }
}

/// Tier 2 (`PD_PAYLOAD_DOCUMENT`).
final class DocumentPayload extends Payload {
  const DocumentPayload(this.ops, {this.codePage = CodePage.pc437});

  final List<DocumentOp> ops;
  final CodePage codePage;

  @override
  PayloadKind get kind => PayloadKind.document;

  @override
  void fillNative(Arena arena, Pointer<PdPayload> out) {
    out.ref.kind = PayloadKind.document.nativeValue;
    final document = out.ref.as.document;
    final array = arena.allocate<PdOp>(sizeOf<PdOp>() * ops.length);
    for (var i = 0; i < ops.length; i++) {
      final op = ops[i];
      final native = array[i];
      native.kind = op._kind;
      native.text = arena.string(op._text);
      native.value = op._value;
    }
    document.ops = array;
    document.count = ops.length;
    document.codePage =
        _requireKnown(codePage.nativeValue, 'codePage', codePage.name);
  }
}

/// Tier 3 (`PD_PAYLOAD_RAW`).
final class RawPayload extends Payload {
  const RawPayload(this.bytes);

  final Uint8List bytes;

  @override
  PayloadKind get kind => PayloadKind.raw;

  @override
  void fillNative(Arena arena, Pointer<PdPayload> out) {
    out.ref.kind = PayloadKind.raw.nativeValue;
    final raw = out.ref.as.raw;
    raw.bytes = arena.bytes(bytes);
    raw.size = bytes.length;
  }
}

// --- M14: cash drawer (docs/cash-drawer.md) -------------------------------------------

/// What a printer's drawer port is, and what is known about it.
///
/// A separate peripheral facet, not part of [DeviceStatus] and not part of the printer's
/// own capabilities: the connector pinout, the drive voltage and the command the firmware
/// implements are three independent facts, and none follows from anything the printer does
/// with paper.
final class DrawerCapabilities {
  const DrawerCapabilities({
    required this.present,
    required this.portStandard,
    required this.voltage,
    required this.maxCurrentMa,
    required this.channelCount,
    required this.sensorPin,
    required this.kickMethod,
    required this.defaultPulseMs,
    required this.maxPulseMs,
    required this.cooldownMs,
    required this.canKickDuringPrint,
    required this.statusAvailable,
    required this.statusMethod,
    required this.sharedBetweenDrawers,
    required this.sharedWithBuzzer,
    required this.electricalProvenance,
    required this.commandsProvenance,
    required this.kickable,
  });

  factory DrawerCapabilities.fromNative(PdDrawerCapabilities native) =>
      DrawerCapabilities(
        present: native.present == 1,
        portStandard: DrawerPortStandard.fromNative(native.standard),
        voltage: native.voltage,
        maxCurrentMa: native.maxCurrentMa,
        channelCount: native.channelCount,
        sensorPin: native.sensorPin,
        kickMethod: DrawerKickMethod.fromNative(native.method),
        defaultPulseMs: native.defaultPulseMs,
        maxPulseMs: native.maxPulseMs,
        cooldownMs: native.cooldownMs,
        canKickDuringPrint: native.canKickDuringPrint == 1,
        statusAvailable: native.statusAvailable == 1,
        statusMethod: DrawerStatusMethod.fromNative(native.statusMethod),
        sharedBetweenDrawers: native.sharedBetweenDrawers == 1,
        sharedWithBuzzer: native.sharedWithBuzzer == 1,
        electricalProvenance: Provenance.fromNative(native.electricalProvenance),
        commandsProvenance: Provenance.fromNative(native.commandsProvenance),
        kickable: native.kickable == 1,
      );

  /// This model has a drawer port at all.
  final bool present;

  /// The electrical classification. Nothing is ever fired on
  /// [DrawerPortStandard.unknown].
  final DrawerPortStandard portStandard;

  /// Volts, or 0 where the manufacturer does not document it — not the same as "low".
  final int voltage;
  final int maxCurrentMa;
  final int channelCount;

  /// 3 on the Epson arrangement, 6 on Star's; 0 when unestablished.
  final int sensorPin;

  final DrawerKickMethod kickMethod;
  final int defaultPulseMs;
  final int maxPulseMs;

  /// Held between two pulses so a retry loop cannot keep a solenoid energised.
  final int cooldownMs;

  /// False on models whose drawer output cannot fire while the mechanism prints; the
  /// pulse is then ordered strictly behind everything already queued.
  final bool canKickDuringPrint;

  final bool statusAvailable;
  final DrawerStatusMethod statusMethod;

  /// Two drive outputs and one switch input: both channels kick independently while the
  /// only readable fact is that *some* attached drawer is open.
  final bool sharedBetweenDrawers;

  /// With the optional external buzzer enabled, the pulse that would fire the drawer
  /// sounds the buzzer instead. Never assume both coexist.
  final bool sharedWithBuzzer;

  /// Deliberately two columns: the XP-S260M's DC 24 V / 1 A output is in Xprinter's own
  /// specification while nothing they publish proves the pulse command.
  final Provenance electricalProvenance;
  final Provenance commandsProvenance;

  /// Whether this SDK may put a pulse on the wire: a method it can drive **and** an
  /// established electrical standard. A caller that reads nothing else should read this.
  final bool kickable;
}

/// The outcome of one opening sequence — a state, never a boolean.
final class DrawerResult {
  const DrawerResult({
    required this.state,
    required this.previousState,
    required this.channel,
    required this.pulseMs,
    required this.elapsedMs,
  });

  factory DrawerResult.fromNative(PdDrawerResult native) => DrawerResult(
        state: DrawerState.fromNative(native.state),
        previousState: DrawerState.fromNative(native.previousState),
        channel: native.channel,
        pulseMs: native.pulseMs,
        elapsedMs: native.elapsedMs,
      );

  final DrawerState state;
  final DrawerState previousState;
  final int channel;

  /// 0 when no pulse was emitted at all: the drawer was already open, or the call was
  /// refused.
  final int pulseMs;

  /// Pulse to verdict — the "sensor changed 143 ms after kick" number. 0 when there was
  /// nothing to wait for.
  final int elapsedMs;

  /// The one thing worth branching on: the switch was seen changing.
  bool get verified => state == DrawerState.openVerified;
}

/// One non-destructive read of the drawer switch.
final class DrawerReading {
  const DrawerReading({
    required this.available,
    required this.answered,
    required this.pinHigh,
    required this.needsCalibration,
    required this.state,
  });

  factory DrawerReading.fromNative(PdDrawerReading native) => DrawerReading(
        available: native.available == 1,
        answered: native.answered == 1,
        pinHigh: switch (native.pinHigh) { 1 => true, 0 => false, _ => null },
        needsCalibration: native.needsCalibration == 1,
        state: DrawerState.fromNative(native.state),
      );

  /// The profile documents a readable switch on this port.
  final bool available;

  /// The device actually replied within the timeout.
  final bool answered;

  /// The raw sense level, or null when nothing answered.
  final bool? pinHigh;

  /// True until this printer's polarity has been measured. While it is true, [state]
  /// stays [DrawerState.unknown] however clear the level is: whether the line reads high
  /// or low when the drawer is open depends on the drawer that is plugged in.
  final bool needsCalibration;

  /// The interpretation, where there is one.
  final DrawerState state;
}

// --- M15: self-test and auto-detection (docs/api.md §15) ------------------------------

/// What a device said about itself, and what that is worth.
///
/// The whole class is evidence, never truth. [trusted] is false until a signal
/// independent of `GS I` agrees with `GS I`, because at least one family ships answering
/// as somebody else's model.
final class DetectedIdentity {
  const DetectedIdentity({
    required this.vendor,
    required this.model,
    required this.firmware,
    required this.serial,
    required this.trusted,
    required this.confidencePercent,
    required this.impersonationSuspected,
    required this.fresh,
  });

  final String vendor;
  final String model;
  final String firmware;
  final String serial;
  final bool trusted;
  final int confidencePercent;
  final bool impersonationSuspected;

  /// Whether this identification came from the call that produced it rather than from
  /// the findings cache.
  final bool fresh;
}

/// The media facts the renderer consumes. Roll width and raster width are separate facts
/// and neither is derived from the other.
final class DetectedMedia {
  const DetectedMedia({
    required this.nominalPaperMm,
    required this.printableWidthDots,
    required this.charsPerLine,
    required this.dpi,
  });

  final int nominalPaperMm;
  final int printableWidthDots;
  final int charsPerLine;
  final int dpi;
}

/// The mechanism, the best grade a job on it can ever claim, who makes that claim, and
/// what the claim rests on.
final class DetectedCompletion {
  const DetectedCompletion({
    required this.mechanism,
    required this.gradeCeiling,
    required this.authority,
    required this.method,
    required this.provenance,
  });

  final CompletionMechanism mechanism;
  final ConfidenceGrade gradeCeiling;
  final CompletionAuthority authority;

  /// The command a support engineer looks up six months later, e.g. `GS(H) fn48`.
  final String method;
  final Provenance provenance;
}

/// The drawer facet, classified rather than fired (docs/cash-drawer.md).
final class DetectedDrawer {
  const DetectedDrawer({
    required this.present,
    required this.kickable,
    required this.portStandard,
    required this.voltage,
    required this.electricalProvenance,
    required this.commandsProvenance,
  });

  final bool present;

  /// Whether this engine may put a pulse on the wire: a drivable method AND an
  /// established electrical standard.
  final bool kickable;
  final DrawerPortStandard portStandard;
  final int voltage;
  final Provenance electricalProvenance;
  final Provenance commandsProvenance;
}

/// The detection report — `pd_detection_summary`.
final class DetectionSummary {
  const DetectionSummary({
    required this.endpoint,
    required this.identity,
    required this.profileId,
    required this.selection,
    required this.media,
    required this.completion,
    required this.drawer,
    required this.degradations,
    required this.provenanceSummary,
  });

  factory DetectionSummary.fromNative(PdDetectionSummary native) {
    final lines = <String>[];
    if (native.degradations != nullptr) {
      for (var index = 0; index < native.degradationCount; index++) {
        lines.add(readNativeString(native.degradations[index]));
      }
    }
    return DetectionSummary(
      endpoint: readNativeString(native.endpoint),
      identity: DetectedIdentity(
        vendor: readNativeString(native.vendor),
        model: readNativeString(native.model),
        firmware: readNativeString(native.firmware),
        serial: readNativeString(native.serial),
        trusted: native.identityTrusted != 0,
        confidencePercent: native.confidencePercent,
        impersonationSuspected: native.impersonationSuspected != 0,
        fresh: native.identityFresh != 0,
      ),
      profileId: readNativeString(native.profileId),
      selection: ProfileSelection.fromNative(native.selection),
      media: DetectedMedia(
        nominalPaperMm: native.nominalPaperMm,
        printableWidthDots: native.printableWidthDots,
        charsPerLine: native.charsPerLine,
        dpi: native.dpi,
      ),
      completion: DetectedCompletion(
        mechanism: CompletionMechanism.fromNative(native.completion),
        gradeCeiling: ConfidenceGrade.fromNative(native.gradeCeiling),
        authority: CompletionAuthority.fromNative(native.authority),
        method: readNativeString(native.method),
        provenance: Provenance.fromNative(native.completionProvenance),
      ),
      drawer: DetectedDrawer(
        present: native.drawerPresent != 0,
        kickable: native.drawerKickable != 0,
        portStandard: DrawerPortStandard.fromNative(native.drawerStandard),
        voltage: native.drawerVoltage,
        electricalProvenance:
            Provenance.fromNative(native.drawerElectricalProvenance),
        commandsProvenance:
            Provenance.fromNative(native.drawerCommandsProvenance),
      ),
      degradations: List.unmodifiable(lines),
      provenanceSummary: readNativeString(native.provenanceSummary),
    );
  }

  final String endpoint;
  final DetectedIdentity identity;
  final String profileId;
  final ProfileSelection selection;
  final DetectedMedia media;
  final DetectedCompletion completion;
  final DetectedDrawer drawer;

  /// Everything requested and not delivered, in the words it is printed in —
  /// `BARCODE not supported on this path` and its relatives.
  final List<String> degradations;

  /// One line for a table row.
  final String provenanceSummary;
}

/// One candidate, classified — `pd_detected_printer`.
final class DetectedPrinter {
  const DetectedPrinter({
    required this.endpoint,
    required this.host,
    required this.port,
    required this.status,
    required this.portOpen,
    required this.fromCache,
    required this.dleEotHex,
    required this.summary,
  });

  factory DetectedPrinter.fromNative(PdDetectedPrinter native) => DetectedPrinter(
        endpoint: readNativeString(native.endpoint),
        host: readNativeString(native.host),
        port: native.port,
        status: DetectionStatus.fromNative(native.status),
        portOpen: native.portOpen != 0,
        fromCache: native.fromCache != 0,
        dleEotHex: readNativeString(native.dleEotHex),
        summary: DetectionSummary.fromNative(native.summary),
      );

  final String endpoint;
  final String host;
  final int port;
  final DetectionStatus status;
  final bool portOpen;

  /// True when the classification came from stored findings rather than from bytes
  /// exchanged in this call.
  final bool fromCache;

  /// Whatever `DLE EOT 1` answered during the sweep, as uppercase hex.
  final String dleEotHex;
  final DetectionSummary summary;
}

/// One address the LAN sweep found listening — `pd_discovered_device`.
final class DiscoveredDevice {
  const DiscoveredDevice({
    required this.ip,
    required this.port,
    required this.portOpen,
    required this.dleEotHex,
  });

  factory DiscoveredDevice.fromNative(PdDiscoveredDevice native) => DiscoveredDevice(
        ip: readNativeString(native.ip),
        port: native.port,
        portOpen: native.port9100Open != 0,
        dleEotHex: readNativeString(native.dleEotHex),
      );

  final String ip;
  final int port;
  final bool portOpen;

  /// `DLE EOT 1`'s answer as uppercase hex, verbatim and unclassified. Empty means the
  /// port accepted the connection and said nothing — a LAN module that does not forward
  /// status bytes, which is a finding and not a failure.
  final String dleEotHex;

  /// Whether anything came back on the backchannel at all.
  bool get answered => dleEotHex.isNotEmpty;
}

/// What one diagnostic ticket established.
///
/// [result] is the proof: the ordinary tri-state outcome of the ordinary engine, so a
/// [JobDone] at [ConfidenceGrade.aJobLevelConfirmation] is the statement that this stack
/// works end to end on this unit.
final class SelfTestResult {
  const SelfTestResult({
    required this.result,
    required this.detection,
    required this.key,
    required this.verificationId,
    required this.ticketLines,
  });

  final JobResult result;
  final DetectionSummary detection;
  final String key;

  /// The four `GS ( H` characters printed as `V:` and inside the QR. Null on a profile
  /// with no wire token to promote.
  final String? verificationId;

  /// The ticket exactly as it was laid out, one entry per line.
  final List<String> ticketLines;
}

// --- M19: the receipt DSL (docs/receipt-dsl.md) --------------------------------------

/// One declared degradation — `pd_report_entry`.
///
/// A missing model path, an unknown formatter and a barcode the profile cannot draw all
/// land here, and the receipt still prints. A declared degradation is not a failure, but
/// it is never silent.
final class ReportEntry {
  const ReportEntry({
    required this.kind,
    required this.block,
    required this.requested,
    required this.delivered,
    required this.path,
    required this.note,
  });

  /// Reads one entry the ABI's index reader filled. The strings are copied here: pd.h
  /// owns them only until the next render on the same driver.
  factory ReportEntry.fromNative(PdReportEntry entry) => ReportEntry(
        kind: ReportKind.fromNative(entry.kind),
        block: readNativeString(entry.block),
        requested: readNativeString(entry.requested),
        delivered: readNativeString(entry.delivered),
        path: RenderPath.fromNative(entry.path),
        note: readNativeString(entry.note),
      );

  final ReportKind kind;

  /// Where it happened, as a path into the document — `blocks[3].cells[1]`, `meta.tz`,
  /// or `document` for one that belongs to no block.
  final String block;

  /// What the document asked for, in the words it is printed in.
  final String requested;

  /// What the paper got. Often `omitted`.
  final String delivered;

  final RenderPath path;

  /// Why, when the pair above does not say it all. Empty when it does.
  final String note;

  @override
  String toString() {
    final because = note.isEmpty ? '' : ' - $note';
    return '$block  ${kind.name}: requested "$requested", delivered '
        '"$delivered" [${path.name}]$because';
  }
}

/// What a document asks the engine for, read back from its `meta` (docs/receipt-dsl.md
/// "Cut control" and "Margins").
///
/// Reported by [RenderedDocument] and applied by `Printer.printDocument` under that
/// document's precedence: what the caller put in [JobOptions] wins, this fills in what
/// the caller left alone, and the printer's profile answers what neither said.
final class DocumentMeta {
  const DocumentMeta({
    required this.cut,
    required this.topFeedDots,
    required this.bottomFeedDots,
  });

  /// Null when the document asked for no particular cut.
  final CutSetting? cut;

  /// Blank paper before the first content line. 0 when the document said nothing.
  final int topFeedDots;

  /// The *total* whitespace between the last content and the cut. The engine feeds
  /// `max(the profile's blade clearance, this)`, so no document can clip its own trailer.
  final int bottomFeedDots;
}

/// A rendered receipt-DSL document: the bytes a printer would receive, and everything
/// that was declared along the way.
final class RenderedDocument {
  const RenderedDocument({
    required this.bytes,
    required this.codePage,
    required this.meta,
    required this.report,
  });

  /// The ESC/POS the renderer produced. Never the whole job: the engine adds its own
  /// initialise, trailing feed, cut and completion fence around this.
  final Uint8List bytes;

  final CodePage codePage;

  final DocumentMeta meta;

  /// Empty when every block rendered exactly as written.
  final List<ReportEntry> report;
}

/// Options for `Printer.renderDocument`. Every default defers to the printer and to the
/// document.
final class RenderDocumentOptions {
  const RenderDocumentOptions({
    this.widthDots = 0,
    this.cutClearanceDots = 0,
    this.maxRowsPerBand = 0,
    this.locale,
    this.currency,
    this.timeZone,
  });

  /// 0 lays the document out for this printer's own configured width, which is what the
  /// engine rasterizes to. Anything else previews a different media width.
  final int widthDots;

  /// Extra whitespace before a **mid-document** `cut` block. The renderer feeds
  /// `max(the profile's blade clearance, this)`: more is always granted, less never.
  final int cutClearanceDots;

  /// 0 means 1024 — the Epson tall-image split, applied to `image` blocks.
  final int maxRowsPerBand;

  /// Overrides the document's own `meta.locale`. Null defers to it.
  final String? locale;

  /// Overrides `meta.currency`.
  final String? currency;

  /// Overrides `meta.tz`. A fixed offset such as `+02:00`; an IANA name is reported as a
  /// [ReportKind.unsupportedTimezone] degradation, because a core that ships no
  /// dependencies ships no timezone database.
  final String? timeZone;

  /// Writes this into a zeroed `pd_render_options`.
  void fillNative(Arena arena, Pointer<PdRenderOptions> out) {
    out.ref
      ..widthDots = widthDots
      ..cutClearanceDots = cutClearanceDots
      ..maxRowsPerBand = maxRowsPerBand
      ..locale = arena.string(locale)
      ..currency = arena.string(currency)
      ..tz = arena.string(timeZone);
  }
}
