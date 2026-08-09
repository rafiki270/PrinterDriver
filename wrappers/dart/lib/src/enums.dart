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

  /// A `GS ( H` echo arrived carrying a token this driver never issued: something else
  /// is writing to the same printer (docs/api.md §14, docs/sdk-spec.md §14).
  ///
  /// The echo is attributed to no job and satisfies no fence. One printer has exactly
  /// one connection owner; this is what a violation of that looks like from the inside,
  /// and it is reported rather than silently misrouted.
  foreignWriterDetected(12),

  /// A `pd_device_event` this build does not know.
  unrecognized(-1);

  const DeviceEvent(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_DEVICE_EVENT_COUNT`.
  static const int nativeCount = 13;

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
/// Carried by `pd_job_result`, so [JobDone.grade] is the core's own answer rather than
/// anything this wrapper derived.
enum ConfidenceGrade {
  /// A durable, queryable printer-side job: ePOS submits, returns a JobID and the
  /// result is retrievable afterwards — the only mechanism in the hierarchy that
  /// survives the application losing its connection between submission and answer
  /// (docs/compatibility-brief.md §24).
  ///
  /// **Nothing produces this grade.** The ePOS transport does not exist in the core, so
  /// no `pd_job_result` can carry it; a profile that reports an ePOS JobID is describing
  /// hardware rather than making a claim, and grades [aJobLevelConfirmation]. It is
  /// mirrored now because these enums are closed and four wrappers mirror them, and
  /// adding a member later would renumber every mirror a second time.
  aPlusDurableQueryableJob(0),

  /// `GS ( H`, Star checked block, documented vendor equivalent.
  aJobLevelConfirmation(1),

  /// `GS r`, vendor idle query.
  bOrderedDeviceResponse(2),

  /// `DLE EOT`, ASB or SNMP taken around the transmission.
  cDeviceStatusAround(3),

  /// A spooler or IPP gateway said completed.
  dSpoolerCompleted(4),

  /// The write succeeded and nothing else is known.
  eTransportOnly(5),

  /// A `pd_confidence_grade` this build does not know.
  unrecognized(-1);

  const ConfidenceGrade(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_GRADE_COUNT`.
  static const int nativeCount = 6;

  /// The letter used in reports; `?` for a grade this build cannot name.
  ///
  /// Strongest first, so `grade.index` and [nativeValue] order the hierarchy the way
  /// §24 states it and two grades compare numerically.
  String get letter => nativeValue < 0
      ? '?'
      : const ['A+', 'A', 'B', 'C', 'D', 'E'][nativeValue];

  static ConfidenceGrade fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// Who is making the claim a [JobResult] carries (docs/device-database.md §D).
///
/// Recorded separately from [ConfidenceGrade] because "completed" from a print server
/// and "completed" from the mechanism that moved the paper are different facts.
enum CompletionAuthority {
  /// The device that moved the paper said so itself.
  physicalPrinter(0),
  vendorSpooler(1),
  pdAgent(2),
  printServer(3),

  /// Nobody said anything; the write succeeded.
  transportOnly(4),

  /// A `pd_completion_authority` this build does not know.
  unrecognized(-1);

  const CompletionAuthority(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_AUTHORITY_COUNT`.
  static const int nativeCount = 5;

  static CompletionAuthority fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// Where the claim that a printer has a capability comes from
/// (docs/compatibility-brief.md §28).
///
/// Recognising ESC/POS print commands does not prove the Epson feedback extensions, so
/// "the manufacturer's manual says so", "we asked the hardware and it answered" and
/// "nobody has checked" are three answers rather than one boolean.
///
/// The three are independent, not ordered: a probe can contradict documentation when the
/// interface path swallows responses, and documentation can cover a model no probe has
/// reached. Comparing two provenances for strength is therefore meaningless, which is
/// why nothing here offers a comparison.
///
/// Mirrors `pd_provenance`.
enum Provenance {
  /// The manufacturer's command documentation lists it.
  documented(0),

  /// This driver asked the installed hardware.
  probed(1),

  /// Neither — a default nobody has confirmed.
  unverified(2),

  /// A `pd_provenance` this build does not know.
  unrecognized(-1);

  const Provenance(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_PROVENANCE_COUNT`.
  static const int nativeCount = 3;

  static Provenance fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// What a device actually speaks (docs/compatibility-brief.md §1).
///
/// Only [escPos] is implemented. The rest exist so that a fleet containing them can be
/// *described* rather than misdriven: a profile naming [zpl], [cpcl], [brotherRaster] or
/// [escP] is refused with [FailureReason.unsupported] before a byte is written, which is
/// the whole difference between a label printer that prints nothing and one that spools
/// a roll of ESC/POS as garbage.
///
/// Mirrors `pd_command_language`.
enum CommandLanguage {
  escPos(0),
  starPrnt(1),
  starLine(2),
  eposXml(3),
  zpl(4),
  cpcl(5),
  brotherRaster(6),

  /// Brother ESC/P — a different language from Epson ESC/POS, despite the name.
  escP(7),

  /// A `pd_command_language` this build does not know.
  unrecognized(-1);

  const CommandLanguage(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_LANGUAGE_COUNT`.
  static const int nativeCount = 8;

  static CommandLanguage fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

// --- M14: cash drawer (docs/cash-drawer.md) -------------------------------------------
//
// The drawer is a separate printer peripheral: its own electrical profile, its own
// command method, its own feedback method, none of them derivable from the others.

/// What is known about a cash drawer. Mirrors `pd_drawer_state`.
///
/// Deliberately not a boolean. Sending `ESC p` and watching the microswitch change are
/// different claims: [kickSentUnverified] is a real answer rather than a softer success,
/// because through a cheap print server the pulse travels forward while the sensor
/// response never comes back.
enum DrawerState {
  /// The switch answered and, with a calibrated polarity, says the drawer is shut.
  closed(0),

  /// Already open. Reading this before a pulse is what stops a redundant kick.
  open(1),

  /// A pulse is on the wire and the verification window has not closed.
  opening(2),

  /// Accepted by the link, and nothing can confirm what happened next.
  kickSentUnverified(3),

  /// The switch was seen changing. The only member that claims physical movement.
  openVerified(4),

  /// The pulse went out and the switch never moved: locked, jammed, wrong channel,
  /// wrong cable — one observation from this side of the connector.
  failedToOpen(5),

  /// No switch input wired or documented on this port. Not an error.
  noSensor(6),

  /// Nothing is known.
  unknown(7),

  /// A `pd_drawer_state` this build does not know.
  unrecognized(-1);

  const DrawerState(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_DRAWER_STATE_COUNT`.
  static const int nativeCount = 8;

  static DrawerState fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// The electrical classification of a drawer port. Mirrors `pd_drawer_port_standard`.
///
/// RJ11/RJ12-looking drawer connectors are not a universal electrical standard: Star's
/// identical-looking 6P6C socket carries +24 V on pin 3 and the sense line on pin 6,
/// precisely where Epson puts sense and signal ground.
enum DrawerPortStandard {
  /// 1 FG, 2 kick 1, 3 sensor, 4 +24 V, 5 kick 2, 6 signal ground.
  epson24V6P6C(0),

  /// The same plug with +24 V on pin 3 and the sense line on pin 6.
  star24V6P6C(1),

  /// The 12 V exceptions, common on 58 mm hardware.
  generic12V6P6C(2),

  /// Unclassified. Nothing is ever energised on one of these.
  unknown(3),

  /// A `pd_drawer_port_standard` this build does not know.
  unrecognized(-1);

  const DrawerPortStandard(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_DRAWER_PORT_STANDARD_COUNT`.
  static const int nativeCount = 4;

  static DrawerPortStandard fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// Which software path fires the drawer. Mirrors `pd_drawer_kick_method`.
///
/// Independent of [DrawerPortStandard]: two printers accepting the same "kick drawer 1"
/// command may still wire the modular socket differently.
enum DrawerKickMethod {
  /// `ESC p m t1 t2`, queued behind print data.
  epsonEscP(0),

  /// The ePOS peripheral API — never raw bytes smuggled into print XML.
  epsonEpos(1),

  /// StarPRNT `appendPeripheral(...)`.
  starPrnt(2),

  /// The Bixolon SDK's `makeDKout`.
  bixolonSdk(3),

  /// `ESC p`, with the "cannot fire while printing" serialisation quirk.
  citizenEscP(4),

  /// `ESC p`; the realtime `DLE DC4` variant exists and is not preferred.
  snbcEscP(5),

  /// Documented, vendor-specific, not implemented here.
  vendor(6),

  /// No drawer path on this device, or none this engine may use.
  unsupported(7),

  /// A `pd_drawer_kick_method` this build does not know.
  unrecognized(-1);

  const DrawerKickMethod(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_DRAWER_KICK_METHOD_COUNT`.
  static const int nativeCount = 8;

  static DrawerKickMethod fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}

/// How the drawer switch is read back. Mirrors `pd_drawer_status_method`.
enum DrawerStatusMethod {
  /// `GS r 2` / `GS r 50` — queued drawer-kick-out connector status.
  gsR2(0),

  /// The drawer bit inside an automatic status back frame.
  asb(1),

  /// Star's `drawerOpenCloseSignal`.
  starSignal(2),

  /// The vendor SDK's own drawer status call.
  vendorSdk(3),

  /// No readable switch.
  none(4),

  /// A `pd_drawer_status_method` this build does not know.
  unrecognized(-1);

  const DrawerStatusMethod(this.nativeValue);

  final int nativeValue;

  /// Mirrors `PD_DRAWER_STATUS_METHOD_COUNT`.
  static const int nativeCount = 5;

  static DrawerStatusMethod fromNative(int value) {
    for (final member in values) {
      if (member.nativeValue == value) return member;
    }
    return unrecognized;
  }
}
