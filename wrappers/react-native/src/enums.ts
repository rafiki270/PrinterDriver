/**
 * The closed enums of the C ABI, mirrored one-for-one.
 *
 * docs/api.md §1.3: "Closed enums everywhere. JobState, ConfidenceLevel, DeviceEvent,
 * FailureReason are defined once in the core and re-exported verbatim by every wrapper.
 * Wrappers cannot add, drop, or partially implement values."
 *
 * Each enum appears twice on purpose:
 *
 *   - as a `const` object mapping the JavaScript spelling to the C value, which is what
 *     crosses the bridge;
 *   - as a string-literal union of the same keys, which is what an app writes and what
 *     makes `switch` exhaustive under `strict`.
 *
 * There are deliberately no TypeScript `enum` declarations here. A TS `enum` is not
 * erasable syntax (so Node cannot run these files directly), it is not a closed set at
 * the type level in the way a literal union is, and its reverse mapping invites
 * `JobState[n]` lookups that silently produce `undefined` for a value this build does
 * not know about. The `fromNative` helpers below make that case explicit instead.
 *
 * Drift is mechanically detectable: test/enums.test.ts compares every table here against
 * src/generated/abi.generated.ts, which scripts/generate-abi-mirror.mjs extracts from
 * capi/include/printerdriver/pd.h.
 */

/** Values reachable across the ABI for the optional (tri-state) integer fields. */
export const PD_UNKNOWN = -1;
export const PD_FALSE = 0;
export const PD_TRUE = 1;

/** A tri-state ABI field: unknown is a real answer, never folded into false. */
export type Tristate = 'unknown' | 'no' | 'yes';

export function tristateFromNative(value: number): Tristate {
  if (value === PD_TRUE) return 'yes';
  if (value === PD_FALSE) return 'no';
  return 'unknown';
}

export function tristateToNative(value: Tristate): number {
  if (value === 'yes') return PD_TRUE;
  if (value === 'no') return PD_FALSE;
  return PD_UNKNOWN;
}

/** Builds the number -> key reverse table for one mirror. */
function reverse<T extends Record<string, number>>(table: T): Map<number, keyof T> {
  const map = new Map<number, keyof T>();
  for (const [key, value] of Object.entries(table)) {
    map.set(value, key as keyof T);
  }
  return map;
}

/**
 * Thrown when the native side reports an enum value this build has no name for. That can
 * only happen when the native library is newer than this package, and it is reported
 * rather than defaulted: a wrapper that quietly mapped an unknown completion grade onto
 * a known one would be inventing evidence.
 */
export class UnknownEnumValueError extends Error {
  readonly enumName: string;
  readonly value: number;

  constructor(enumName: string, value: number) {
    super(
      `${enumName}: the native library reported value ${value}, which this build of ` +
        `printerdriver-react-native does not know. The native library is newer than the ` +
        `JavaScript package; upgrade the package rather than interpreting the value.`
    );
    this.name = 'UnknownEnumValueError';
    this.enumName = enumName;
    this.value = value;
  }
}

function decoder<T extends Record<string, number>>(enumName: string, table: T) {
  const map = reverse(table);
  return (value: number): keyof T => {
    const key = map.get(value);
    if (key === undefined) throw new UnknownEnumValueError(enumName, value);
    return key;
  };
}

// --- pd_job_state ---------------------------------------------------------------------

/** The job state machine of docs/techspec.md §5.1, exactly. */
export const JobState = {
  queued: 0,
  preflightOk: 1,
  sendStarted: 2,
  bytesSent: 3,
  printConfirmed: 4,
  cutCommandProcessed: 5,
  doneSoftware: 6,
  physicallyVerified: 7,
  failedKnown: 8,
  unknown: 9,
  /** Produced only by the print-queue addon (docs/sdk-spec.md §12). */
  heldOffline: 10,
} as const;
export type JobState = keyof typeof JobState;
export const jobStateFromNative = decoder('pd_job_state', JobState);

// --- pd_confidence_level --------------------------------------------------------------

/** What evidence backs the current claim. Never inflated. */
export const ConfidenceLevel = {
  transportAccepted: 0,
  printerHealthy: 1,
  printConfirmed: 2,
  cutProcessed: 3,
  cutFaultFree: 4,
  physicallyVerified: 5,
} as const;
export type ConfidenceLevel = keyof typeof ConfidenceLevel;
export const confidenceLevelFromNative = decoder('pd_confidence_level', ConfidenceLevel);

// --- pd_device_event ------------------------------------------------------------------

/** The per-printer stream that replaces availability polling. */
export const DeviceEvent = {
  online: 0,
  offline: 1,
  coverOpen: 2,
  coverClosed: 3,
  paperOut: 4,
  paperNearEnd: 5,
  paperOk: 6,
  cutterError: 7,
  recoverableError: 8,
  unrecoverableError: 9,
  connectionLost: 10,
  connectionRestored: 11,
  /** A GS ( H echo carrying a token this driver never issued (docs/api.md §14). */
  foreignWriterDetected: 12,
} as const;
export type DeviceEvent = keyof typeof DeviceEvent;
export const deviceEventFromNative = decoder('pd_device_event', DeviceEvent);

// --- pd_failure_reason ----------------------------------------------------------------

export const FailureReason = {
  none: 0,
  transportUnreachable: 1,
  preflightCoverOpen: 2,
  preflightPaperOut: 3,
  preflightHardwareError: 4,
  timeoutAwaitingCompletion: 5,
  cutterFault: 6,
  unsupported: 7,
  unknown: 8,
  /** Print-queue addon only. */
  expired: 9,
  /** Print-queue addon only. */
  queueOverflow: 10,
} as const;
export type FailureReason = keyof typeof FailureReason;
export const failureReasonFromNative = decoder('pd_failure_reason', FailureReason);

// --- pd_job_outcome -------------------------------------------------------------------

/**
 * Deliberately tri-state (docs/api.md §1.4). There is no success boolean anywhere in
 * this package: collapsing `unknown` into either bucket is the bug that produces
 * duplicate kitchen tickets.
 */
export const JobOutcome = {
  done: 0,
  failed: 1,
  unknown: 2,
} as const;
export type JobOutcome = keyof typeof JobOutcome;
export const jobOutcomeFromNative = decoder('pd_job_outcome', JobOutcome);

// --- pd_confidence_grade --------------------------------------------------------------

/** What *class* of evidence a claim rests on. Orthogonal to ConfidenceLevel. */
export const ConfidenceGrade = {
  /** A durable, queryable printer-side job (ePOS JobID, retrieved result). */
  aplusDurableQueryableJob: 0,
  /** GS ( H, Star checked block. */
  aJobLevelConfirmation: 1,
  /** GS r, vendor idle query. */
  bOrderedDeviceResponse: 2,
  /** DLE EOT / ASB / SNMP around the send. */
  cDeviceStatusAround: 3,
  /** A spooler or IPP gateway said completed. */
  dSpoolerCompleted: 4,
  /** The write succeeded, nothing else is known. */
  eTransportOnly: 5,
} as const;
export type ConfidenceGrade = keyof typeof ConfidenceGrade;
export const confidenceGradeFromNative = decoder('pd_confidence_grade', ConfidenceGrade);

/** "A+", "A".."E" — the letter a report tabulates. Mirrors pd_confidence_grade_letter. */
export const confidenceGradeLetters: Readonly<Record<ConfidenceGrade, string>> = {
  aplusDurableQueryableJob: 'A+',
  aJobLevelConfirmation: 'A',
  bOrderedDeviceResponse: 'B',
  cDeviceStatusAround: 'C',
  dSpoolerCompleted: 'D',
  eTransportOnly: 'E',
};

// --- pd_provenance --------------------------------------------------------------------

/** Where the claim that a printer has a capability comes from. Not an ordering. */
export const Provenance = {
  documented: 0,
  probed: 1,
  unverified: 2,
} as const;
export type Provenance = keyof typeof Provenance;
export const provenanceFromNative = decoder('pd_provenance', Provenance);

// --- pd_command_language --------------------------------------------------------------

/** Only `escPos` is implemented; the rest exist so a fleet containing them can be
 * described rather than misdriven. */
export const CommandLanguage = {
  escPos: 0,
  starPrnt: 1,
  starLine: 2,
  eposXml: 3,
  zpl: 4,
  cpcl: 5,
  brotherRaster: 6,
  /** Brother ESC/P — a different language from Epson ESC/POS. */
  escP: 7,
} as const;
export type CommandLanguage = keyof typeof CommandLanguage;
export const commandLanguageFromNative = decoder('pd_command_language', CommandLanguage);

// --- pd_completion_authority ----------------------------------------------------------

/** Who is actually making the claim. */
export const CompletionAuthority = {
  physicalPrinter: 0,
  vendorSpooler: 1,
  pdAgent: 2,
  printServer: 3,
  transportOnly: 4,
} as const;
export type CompletionAuthority = keyof typeof CompletionAuthority;
export const completionAuthorityFromNative = decoder(
  'pd_completion_authority',
  CompletionAuthority
);

// --- pd_cut ---------------------------------------------------------------------------

/** `profile` means "whatever this printer's cutter does". */
export const CutSetting = {
  profile: 0,
  partial: 1,
  full: 2,
  none: 3,
} as const;
export type CutSetting = keyof typeof CutSetting;
export const cutSettingFromNative = decoder('pd_cut', CutSetting);

// --- pd_preflight ---------------------------------------------------------------------

export const PreflightMode = {
  strict: 0,
  skip: 1,
} as const;
export type PreflightMode = keyof typeof PreflightMode;
export const preflightModeFromNative = decoder('pd_preflight', PreflightMode);

// --- pd_payload_kind ------------------------------------------------------------------

export const PayloadKind = {
  rasterRgba8: 0,
  document: 1,
  raw: 2,
} as const;
export type PayloadKind = keyof typeof PayloadKind;
export const payloadKindFromNative = decoder('pd_payload_kind', PayloadKind);

// --- pd_completion_mechanism ----------------------------------------------------------

/** Which ordered fence a profile answers. */
export const CompletionMechanism = {
  gsParenH: 0,
  gsR1: 1,
  vendorIdle: 2,
  eposJobId: 3,
  starCheckedBlock: 4,
  none: 5,
  starEtb: 6,
  starEscGsEtx: 7,
} as const;
export type CompletionMechanism = keyof typeof CompletionMechanism;
export const completionMechanismFromNative = decoder(
  'pd_completion_mechanism',
  CompletionMechanism
);

// --- pd_cut_variant -------------------------------------------------------------------

/** The cut a profile's mechanism actually performs. */
export const CutVariant = {
  partial: 0,
  full: 1,
  none: 2,
} as const;
export type CutVariant = keyof typeof CutVariant;
export const cutVariantFromNative = decoder('pd_cut_variant', CutVariant);

// --- pd_alignment ---------------------------------------------------------------------

/** Values are the `n` operand of ESC a n. */
export const Alignment = {
  left: 0,
  center: 1,
  right: 2,
} as const;
export type Alignment = keyof typeof Alignment;
export const alignmentFromNative = decoder('pd_alignment', Alignment);

// --- pd_code_page ---------------------------------------------------------------------

/**
 * Values are the `n` operand of ESC t n, so they are NOT contiguous and NOT indices.
 * `codePageOrder` is the member order pd_code_page_at() iterates.
 */
export const CodePage = {
  pc437: 0,
  pc850: 2,
  wpc1252: 16,
  pc852: 18,
  pc858: 19,
} as const;
export type CodePage = keyof typeof CodePage;
export const codePageFromNative = decoder('pd_code_page', CodePage);
export const codePageOrder: readonly CodePage[] = ['pc437', 'pc850', 'wpc1252', 'pc852', 'pc858'];

// --- pd_binarization ------------------------------------------------------------------

/** How the raster tier turns grey into dots. */
export const Binarization = {
  fixedThreshold: 0,
  floydSteinberg: 1,
} as const;
export type Binarization = keyof typeof Binarization;
export const binarizationFromNative = decoder('pd_binarization', Binarization);

// --- pd_op_kind -----------------------------------------------------------------------

/** Tier 2 document operations. */
export const OpKind = {
  text: 0,
  line: 1,
  align: 2,
  bold: 3,
  feed: 4,
} as const;
export type OpKind = keyof typeof OpKind;
export const opKindFromNative = decoder('pd_op_kind', OpKind);

// --- pd_drain_order -------------------------------------------------------------------

export const DrainOrder = {
  /** Submission order — the safe default for sequenced tickets. */
  fifo: 0,
  priority: 1,
} as const;
export type DrainOrder = keyof typeof DrainOrder;
export const drainOrderFromNative = decoder('pd_drain_order', DrainOrder);

// --- pd_drawer_state ------------------------------------------------------------------

/** docs/cash-drawer.md "API states (never a boolean)". */
export const DrawerState = {
  closed: 0,
  open: 1,
  opening: 2,
  /** The link accepted the pulse and nothing can confirm what happened next. */
  kickSentUnverified: 3,
  /** The only member that claims physical movement. */
  openVerified: 4,
  failedToOpen: 5,
  /** This port has no switch input wired or documented. Not an error. */
  noSensor: 6,
  unknown: 7,
} as const;
export type DrawerState = keyof typeof DrawerState;
export const drawerStateFromNative = decoder('pd_drawer_state', DrawerState);

// --- pd_drawer_port_standard ----------------------------------------------------------

/** A cable that fits is not electrically correct. */
export const DrawerPortStandard = {
  epson24v6p6c: 0,
  star24v6p6c: 1,
  generic12v6p6c: 2,
  unknown: 3,
} as const;
export type DrawerPortStandard = keyof typeof DrawerPortStandard;
export const drawerPortStandardFromNative = decoder(
  'pd_drawer_port_standard',
  DrawerPortStandard
);

// --- pd_drawer_kick_method ------------------------------------------------------------

export const DrawerKickMethod = {
  epsonEscP: 0,
  epsonEpos: 1,
  starPrnt: 2,
  bixolonSdk: 3,
  citizenEscP: 4,
  snbcEscP: 5,
  vendor: 6,
  unsupported: 7,
} as const;
export type DrawerKickMethod = keyof typeof DrawerKickMethod;
export const drawerKickMethodFromNative = decoder('pd_drawer_kick_method', DrawerKickMethod);

// --- pd_drawer_status_method ----------------------------------------------------------

export const DrawerStatusMethod = {
  gsR2: 0,
  asb: 1,
  starSignal: 2,
  vendorSdk: 3,
  none: 4,
} as const;
export type DrawerStatusMethod = keyof typeof DrawerStatusMethod;
export const drawerStatusMethodFromNative = decoder(
  'pd_drawer_status_method',
  DrawerStatusMethod
);

// --- pd_profile_selection -------------------------------------------------------------

/** How the capability profile in force was arrived at. Not a provenance. */
export const ProfileSelection = {
  documented: 0,
  probed: 1,
  /** The shipped default is the whole truth: UNKNOWN DEVICE, not ordinary device. */
  default: 2,
} as const;
export type ProfileSelection = keyof typeof ProfileSelection;
export const profileSelectionFromNative = decoder('pd_profile_selection', ProfileSelection);

// --- pd_detection_status --------------------------------------------------------------

export const DetectionStatus = {
  answered: 0,
  /** The port accepted the connection and said nothing. A finding, never a failure. */
  silent: 1,
  /** Reachable and deliberately not interrogated. Never "no capabilities": nobody asked. */
  unverified: 2,
  unreachable: 3,
} as const;
export type DetectionStatus = keyof typeof DetectionStatus;
export const detectionStatusFromNative = decoder('pd_detection_status', DetectionStatus);

// --- pd_match_kind --------------------------------------------------------------------

/** A custom completion matcher's verdict on the bytes it was handed. */
export const MatchKind = {
  /** A fence answer carrying `token`; confirm like a GS ( H echo. */
  matched: 0,
  /** Not this mechanism's bytes; the core drops its matcher buffer. */
  notMine: 1,
  /** An answer may be forming but is incomplete; keep buffering. */
  needMore: 2,
} as const;
export type MatchKind = keyof typeof MatchKind;
export const matchKindFromNative = decoder('pd_match_kind', MatchKind);

// --- the table the enum-bridge test walks ----------------------------------------------

/**
 * Every mirror above, keyed by its pd.h enum name. test/enums.test.ts iterates this
 * against src/generated/abi.generated.ts, so adding a mirror here without adding it to
 * pd.h (or the reverse) fails the suite.
 */
export const enumMirrors: Readonly<Record<string, Readonly<Record<string, number>>>> = {
  pd_job_state: JobState,
  pd_confidence_level: ConfidenceLevel,
  pd_device_event: DeviceEvent,
  pd_failure_reason: FailureReason,
  pd_job_outcome: JobOutcome,
  pd_confidence_grade: ConfidenceGrade,
  pd_provenance: Provenance,
  pd_command_language: CommandLanguage,
  pd_completion_authority: CompletionAuthority,
  pd_cut: CutSetting,
  pd_preflight: PreflightMode,
  pd_payload_kind: PayloadKind,
  pd_completion_mechanism: CompletionMechanism,
  pd_cut_variant: CutVariant,
  pd_alignment: Alignment,
  pd_code_page: CodePage,
  pd_binarization: Binarization,
  pd_op_kind: OpKind,
  pd_drain_order: DrainOrder,
  pd_drawer_state: DrawerState,
  pd_drawer_port_standard: DrawerPortStandard,
  pd_drawer_kick_method: DrawerKickMethod,
  pd_drawer_status_method: DrawerStatusMethod,
  pd_profile_selection: ProfileSelection,
  pd_detection_status: DetectionStatus,
  pd_match_kind: MatchKind,
};
