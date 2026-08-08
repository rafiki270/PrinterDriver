/// The closed enums of `capi/include/printerdriver/pd.h`, mirrored member for member.
///
/// Every enum here re-exports a C enum that itself mirrors a C++ enum in
/// `core/include/printerdriver`, with the mirroring enforced by `static_assert` in the
/// library's own translation unit (docs/api.md §1.3). The Dart side therefore carries
/// the native value explicitly rather than relying on declaration order, and every
/// mapping goes through `fromNative`, which never guesses: a value this build does not
/// know becomes [unrecognized] and keeps its raw number, so an SDK upgrade that adds a
/// member surfaces as an unhandled case instead of as silently wrong printing.
library;

/// Thrown when the native library reports an enum value that this build of the wrapper
/// cannot map and the surrounding API has no honest way to carry it.
///
/// The only place this is raised is `pd_job_outcome`: the tri-state result is a sealed
/// type with exactly three cases (docs/api.md §4), so an unknown fourth outcome cannot
/// be represented at all — and collapsing it into any of the three is precisely the bug
/// the tri-state exists to prevent.
final class UnrecognizedNativeValue implements Exception {
  UnrecognizedNativeValue(this.enumName, this.value);

  /// The C enum that produced the value, e.g. `pd_job_outcome`.
  final String enumName;

  /// The raw number the native library returned.
  final int value;

  @override
  String toString() =>
      'UnrecognizedNativeValue: $enumName = $value is not a member this build of '
      'package:printerdriver knows. The native library is newer than the wrapper; '
      'upgrade the wrapper rather than interpreting the value.';
}

/// The recorded position of a job in the state machine of docs/techspec.md §5.1.
///
/// Mirrors `pd_job_state`.
enum JobState {
  queued(0),
  preflightOk(1),
  sendStarted(2),
  bytesSent(3),
  printConfirmed(4),
  cutCommandProcessed(5),
  doneSoftware(6),
  physicallyVerified(7),
  failedKnown(8),
  unknown(9),

  /// Produced only by the print-queue addon (docs/sdk-spec.md §12).
  heldOffline(10),

  /// A `pd_job_state` this build does not know. Never produced by a native library
  /// built from the same revision as this package.
  unrecognized(-1);

  const JobState(this.nativeValue);

  /// The `pd_job_state` value, not the declaration index.
  final int nativeValue;

  /// Number of real members, mirroring `PD_JOB_STATE_COUNT`. Excludes
  /// [JobState.unrecognized].
  static const int nativeCount = 11;

  static JobState fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }

  /// Whether the job can still move. Mirrors what `pd_job_is_terminal` reports, but
  /// derived from a state rather than queried, so it is only ever used on a recorded
  /// event; the driver itself always asks the ABI.
  bool get isTerminal =>
      this == doneSoftware ||
      this == physicallyVerified ||
      this == failedKnown ||
      this == unknown;
}

/// What evidence backs the current claim. Never inflated by the SDK.
///
/// Mirrors `pd_confidence_level`.
enum ConfidenceLevel {
  /// The write succeeded and nothing else is known.
  transportAccepted(0),

  /// A status query answered healthy around the transmission.
  printerHealthy(1),

  /// The printer confirmed it processed the print data.
  printConfirmed(2),

  /// The cut command was processed.
  cutProcessed(3),

  /// The cut was processed and the cutter reported no fault afterwards.
  cutFaultFree(4),

  /// Verified by something that watched the paper, not by the printer's own claim.
  physicallyVerified(5),

  /// A `pd_confidence_level` this build does not know.
  unrecognized(-1);

  const ConfidenceLevel(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_CONFIDENCE_COUNT`.
  static const int nativeCount = 6;

  static ConfidenceLevel fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// The per-printer stream that replaces availability polling (docs/api.md §4).
///
/// Mirrors `pd_device_event`.
enum DeviceEvent {
  online(0),
  offline(1),
  coverOpen(2),
  coverClosed(3),
  paperOut(4),
  paperNearEnd(5),
  paperOk(6),
  cutterError(7),
  recoverableError(8),
  unrecoverableError(9),
  connectionLost(10),
  connectionRestored(11),

  /// A `pd_device_event` this build does not know.
  unrecognized(-1);

  const DeviceEvent(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_DEVICE_EVENT_COUNT`.
  static const int nativeCount = 12;

  static DeviceEvent fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// Why a job failed, or how far a job that ended [JobOutcomeUnknown] got.
///
/// Mirrors `pd_failure_reason`.
enum FailureReason {
  none(0),
  transportUnreachable(1),
  preflightCoverOpen(2),
  preflightPaperOut(3),
  preflightHardwareError(4),
  timeoutAwaitingCompletion(5),
  cutterFault(6),
  unsupported(7),
  unknown(8),

  /// Print-queue addon only (docs/sdk-spec.md §12).
  expired(9),

  /// Print-queue addon only (docs/sdk-spec.md §12).
  queueOverflow(10),

  /// A `pd_failure_reason` this build does not know.
  unrecognized(-1);

  const FailureReason(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_REASON_COUNT`.
  static const int nativeCount = 11;

  static FailureReason fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// The cut a job asks for. [CutSetting.profile] means "whatever this printer's cutter
/// does".
///
/// Mirrors `pd_cut`.
enum CutSetting {
  profile(0),
  partial(1),
  full(2),
  none(3),

  /// A `pd_cut` this build does not know.
  unrecognized(-1);

  const CutSetting(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_CUT_COUNT`.
  static const int nativeCount = 4;

  static CutSetting fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// Whether a job refuses to start on a printer that reports cover-open or paper-out.
///
/// Mirrors `pd_preflight`.
enum PreflightMode {
  strict(0),
  skip(1),

  /// A `pd_preflight` this build does not know.
  unrecognized(-1);

  const PreflightMode(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_PREFLIGHT_COUNT`.
  static const int nativeCount = 2;

  static PreflightMode fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// The tier a job was submitted as (docs/api.md §3).
///
/// Mirrors `pd_payload_kind`.
enum PayloadKind {
  rasterRgba8(0),
  document(1),
  raw(2),

  /// A `pd_payload_kind` this build does not know.
  unrecognized(-1);

  const PayloadKind(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_PAYLOAD_KIND_COUNT`.
  static const int nativeCount = 3;

  static PayloadKind fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// Which ordered fence a printer's capability profile answers.
///
/// Mirrors `pd_completion_mechanism`.
enum CompletionMechanism {
  /// `GS ( H` process-ID markers: job-level confirmation.
  gsParenH(0),

  /// Queued `GS r 1`: an ordered device response, not a job-level one.
  gsR1(1),

  /// Vendor "working state"/idle query; profile data only.
  vendorIdle(2),

  /// ePOS JobID + queryable print result; profile data only.
  eposJobId(3),

  /// StarPRNT begin/endCheckedBlock; profile data only.
  starCheckedBlock(4),

  /// Write-only. Nothing above [ConfidenceLevel.transportAccepted] is claimable.
  none(5),

  /// A `pd_completion_mechanism` this build does not know.
  unrecognized(-1);

  const CompletionMechanism(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_COMPLETION_COUNT`.
  static const int nativeCount = 6;

  static CompletionMechanism fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// The cut a profile's mechanism actually performs.
///
/// Mirrors `pd_cut_variant`.
enum CutVariant {
  partial(0),
  full(1),
  none(2),

  /// A `pd_cut_variant` this build does not know.
  unrecognized(-1);

  const CutVariant(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_CUT_VARIANT_COUNT`.
  static const int nativeCount = 3;

  static CutVariant fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// Text alignment in the document tier. Values are the `n` operand of `ESC a n`.
///
/// Mirrors `pd_alignment`.
enum Alignment {
  left(0),
  center(1),
  right(2),

  /// A `pd_alignment` this build does not know.
  unrecognized(-1);

  const Alignment(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_ALIGN_COUNT`.
  static const int nativeCount = 3;

  static Alignment fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// The code page a document is transliterated into.
///
/// Mirrors `pd_code_page`. The values are the `n` operand of `ESC t n`, so they are not
/// contiguous and [nativeCount] is the number of supported members, not a past-the-end
/// sentinel — exactly as in pd.h, where iteration goes through `pd_code_page_at()`.
enum CodePage {
  pc437(0),
  pc850(2),
  wpc1252(16),
  pc852(18),
  pc858(19),

  /// A `pd_code_page` this build does not know.
  unrecognized(-1);

  const CodePage(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_CODE_PAGE_COUNT`: the member count, not the largest value.
  static const int nativeCount = 5;

  static CodePage fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// How the raster tier turns grey into dots.
///
/// Mirrors `pd_binarization`.
enum Binarization {
  fixedThreshold(0),
  floydSteinberg(1),

  /// A `pd_binarization` this build does not know.
  unrecognized(-1);

  const Binarization(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_BINARIZATION_COUNT`.
  static const int nativeCount = 2;

  static Binarization fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// The class of evidence a completion claim rests on (docs/api.md §13,
/// docs/device-database.md "Confidence grades for every route").
///
/// Orthogonal to [ConfidenceLevel]: the level says how far up the ladder a job climbed,
/// the grade says what kind of evidence the claim is made of.
///
/// Not carried by `pd_job_result` in the C ABI at this revision, so [JobDone.grade] is
/// currently always null; the enum exists so the shape does not change when the ABI
/// grows the field.
enum ConfidenceGrade {
  /// `GS ( H`, ePOS JobID result, Star checked block.
  aJobLevelConfirmation(0),

  /// `GS r`, vendor idle query.
  bOrderedDeviceResponse(1),

  /// `DLE EOT`, ASB or SNMP taken around the transmission.
  cDeviceStatusAround(2),

  /// A spooler or IPP gateway said completed.
  dSpoolerCompleted(3),

  /// The write succeeded and nothing else is known.
  eTransportOnly(4);

  const ConfidenceGrade(this.nativeValue);

  final int nativeValue;

  /// The letter used in reports.
  String get letter => const ['A', 'B', 'C', 'D', 'E'][nativeValue];
}

/// Who is making the claim a [JobResult] carries (docs/device-database.md §D).
///
/// Not carried by `pd_job_result` in the C ABI at this revision; see [ConfidenceGrade].
enum CompletionAuthority {
  physicalPrinter(0),
  vendorSpooler(1),
  pdAgent(2),
  printServer(3),
  transportOnly(4);

  const CompletionAuthority(this.nativeValue);

  final int nativeValue;
}
