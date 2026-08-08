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

  /// Reads a `pd_job_event` the ABI is handing over.
  ///
  /// Only ever called while the pointer is known to be alive — during the synchronous
  /// replay inside `pd_subscribe_job`. See [PrintJob] for why that matters.
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
  const JobResult({required this.confidence});

  /// How far up the evidence ladder the job got.
  ///
  /// Carried on all three outcomes, not only on [JobDone]: on a failure or an unknown
  /// it is what an operator needs in order to decide about a reprint.
  final ConfidenceLevel confidence;
}

/// The job reached `DoneSoftware` (or `PhysicallyVerified`).
final class JobDone extends JobResult {
  const JobDone({
    required super.confidence,
    this.grade,
    this.authority,
    this.method,
  });

  /// The class of evidence behind the claim (docs/api.md §13).
  ///
  /// Null against the current C ABI: `pd_job_result` carries `outcome`, `confidence`
  /// and `reason` and nothing else, and this wrapper does not manufacture a grade from
  /// the confidence level — deriving one here is the kind of decision pd.h keeps in the
  /// core. Use [Printer.completion] for the fence the printer actually answers.
  final ConfidenceGrade? grade;

  /// Who is making the claim. Null for the same reason as [grade].
  final CompletionAuthority? authority;

  /// The command behind the claim, e.g. `GS(H) fn48`. Null for the same reason as
  /// [grade].
  final String? method;

  @override
  String toString() => 'JobDone(${confidence.name})';
}

/// The job failed and the failure is confirmed: nothing printed, or the failure itself
/// was reported.
final class JobFailed extends JobResult {
  const JobFailed({required super.confidence, required this.reason});

  final FailureReason reason;

  @override
  String toString() => 'JobFailed(${reason.name}, reached ${confidence.name})';
}

/// Bytes were sent and never acknowledged: a timeout, a dropped link, a crash.
///
/// Not a success and not a failure. Surface it to an operator; resolve it with
/// `forceReprint` or with a manual confirmation.
final class JobUnknown extends JobResult {
  const JobUnknown({required super.confidence, required this.reason});

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
  return switch (result.outcome) {
    0 => JobDone(confidence: confidence),
    1 => JobFailed(confidence: confidence, reason: reason),
    2 => JobUnknown(confidence: confidence, reason: reason),
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

  /// Writes this into a zeroed `pd_job_options`.
  void fillNative(Arena arena, Pointer<PdJobOptions> out) {
    final options = out.ref;
    options.key = arena.string(key);
    options.cut = _requireKnown(cut.nativeValue, 'cut', cut.name);
    options.openDrawer = openDrawer ? 1 : 0;
    options.preflight =
        _requireKnown(preflight.nativeValue, 'preflight', preflight.name);
    options.timeoutMs = timeout?.inMilliseconds ?? 0;
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
