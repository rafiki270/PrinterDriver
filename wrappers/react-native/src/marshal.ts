/**
 * Pure translation between the app-facing types of src/types.ts and the plain objects the
 * TurboModule exchanges with C++. Nothing here touches the native module, which is what
 * makes it all directly testable on a plain Node host — see test/marshal.test.ts.
 *
 * The wire shapes below are the contract cpp/PrinterDriverModule.cpp reads and writes.
 * Field names are the C struct field names in camelCase; enum fields are the C integer
 * values. Every "all-zeroes is the documented default" promise in pd.h is honoured here
 * by emitting 0 for an omitted field rather than inventing a wrapper-level default — the
 * defaults live in the core, and a wrapper that duplicated them would be a second place
 * for them to drift.
 *
 * The two INVERTED flags (`suppressVerificationId`, `suppressBanner`) are inverted here
 * and nowhere else: an app writes `printVerificationId: true` / `banner: true`, and the
 * ABI's all-zeroes-means-print-the-evidence rule is preserved on the wire.
 */

import {
  Alignment,
  Binarization,
  CodePage,
  CutSetting,
  DrainOrder,
  OpKind,
  PreflightMode,
  completionAuthorityFromNative,
  completionMechanismFromNative,
  confidenceGradeFromNative,
  confidenceLevelFromNative,
  detectionStatusFromNative,
  drawerKickMethodFromNative,
  drawerPortStandardFromNative,
  drawerStateFromNative,
  drawerStatusMethodFromNative,
  failureReasonFromNative,
  jobOutcomeFromNative,
  jobStateFromNative,
  profileSelectionFromNative,
  provenanceFromNative,
  codePageFromNative,
  cutSettingFromNative,
  renderPathFromNative,
  reportKindFromNative,
  tristateFromNative,
} from './enums.ts';
import type {
  AutoDetectOptions,
  DetectedPrinter,
  DetectionSummary,
  DeviceStatus,
  DiscoverOptions,
  DiscoveredDevice,
  DocumentOp,
  DrawerCapabilities,
  DrawerReading,
  DrawerRequest,
  DrawerResult,
  DriverConfig,
  JobEvent,
  JobOptions,
  JobResult,
  JsonDocument,
  Payload,
  QueueOptions,
  QueuePolicy,
  RenderOptions,
  RenderedDocument,
  ReportEntry,
  ReprintOptions,
  SelfTestOptions,
  TcpPrinterConfig,
} from './types.ts';

// --- helpers -----------------------------------------------------------------------------

const bool = (value: boolean | undefined): number => (value === true ? 1 : 0);
const int = (value: number | undefined): number => (value === undefined ? 0 : value);
const str = (value: string | undefined): string => (value === undefined ? '' : value);
/** Inverted flags: absent or true means "do it", so the suppress bit is 0. */
const suppress = (value: boolean | undefined): number => (value === false ? 1 : 0);

// --- outbound: configuration ---------------------------------------------------------------

export interface NativeDriverConfig {
  storageDirectory: string;
  fsyncDisabled: number;
  /** Whether the JS side installed an `onLog`; the native side wires pd_config.log to it. */
  hasLog: number;
}

export function marshalDriverConfig(config: DriverConfig | undefined): NativeDriverConfig {
  return {
    storageDirectory: str(config?.storageDirectory),
    fsyncDisabled: bool(config?.fsyncDisabled),
    hasLog: bool(config?.onLog !== undefined),
  };
}

export interface NativeTcpConfig {
  printerId: string;
  host: string;
  port: number;
  widthDots: number;
  profileId: string;
  connectTimeoutMs: number;
}

export function marshalTcpConfig(config: TcpPrinterConfig): NativeTcpConfig {
  return {
    printerId: str(config.printerId),
    host: config.host,
    port: int(config.port),
    widthDots: int(config.widthDots),
    profileId: str(config.profileId),
    connectTimeoutMs: int(config.connectTimeoutMs),
  };
}

// --- outbound: job options -------------------------------------------------------------------

export interface NativeJobOptions {
  key: string;
  cut: number;
  openDrawer: number;
  preflight: number;
  timeoutMs: number;
  topFeedDots: number;
  bottomFeedDots: number;
  suppressVerificationId: number;
}

export function marshalJobOptions(options: JobOptions | undefined): NativeJobOptions {
  return {
    key: str(options?.key),
    cut: CutSetting[options?.cut ?? 'profile'],
    openDrawer: bool(options?.openDrawer),
    preflight: PreflightMode[options?.preflight ?? 'strict'],
    timeoutMs: int(options?.timeoutMs),
    topFeedDots: int(options?.topFeedDots),
    bottomFeedDots: int(options?.bottomFeedDots),
    suppressVerificationId: suppress(options?.printVerificationId),
  };
}

export interface NativeReprintOptions {
  job: NativeJobOptions;
  suppressBanner: number;
}

export function marshalReprintOptions(
  options: ReprintOptions | undefined
): NativeReprintOptions {
  return { job: marshalJobOptions(options), suppressBanner: suppress(options?.banner) };
}

export interface NativeQueuePolicy {
  holdWhileOffline: number;
  defaultTtlMs: number;
  maxDepth: number;
  drainOrder: number;
}

export function marshalQueuePolicy(policy: QueuePolicy | undefined): NativeQueuePolicy {
  return {
    holdWhileOffline: bool(policy?.holdWhileOffline),
    defaultTtlMs: int(policy?.defaultTtlMs),
    maxDepth: int(policy?.maxDepth),
    drainOrder: DrainOrder[policy?.drainOrder ?? 'fifo'],
  };
}

export interface NativeQueueOptions {
  key: string;
  ttlMs: number;
  priority: number;
  cut: number;
  openDrawer: number;
  preflight: number;
  timeoutMs: number;
}

export function marshalQueueOptions(options: QueueOptions | undefined): NativeQueueOptions {
  return {
    key: str(options?.key),
    ttlMs: int(options?.ttlMs),
    priority: int(options?.priority),
    cut: CutSetting[options?.cut ?? 'profile'],
    openDrawer: bool(options?.openDrawer),
    preflight: PreflightMode[options?.preflight ?? 'strict'],
    timeoutMs: int(options?.timeoutMs),
  };
}

export interface NativeDrawerRequest {
  channel: number;
  pulseMs: number;
}

export function marshalDrawerRequest(request: DrawerRequest | undefined): NativeDrawerRequest {
  return { channel: int(request?.channel), pulseMs: int(request?.pulseMs) };
}

export interface NativeSelfTestOptions {
  key: string;
  refreshIdentity: number;
  probeWithoutPrinting: number;
  noBarcode: number;
  barcodeData: string;
  noVerificationId: number;
  timeoutMs: number;
}

export function marshalSelfTestOptions(
  options: SelfTestOptions | undefined
): NativeSelfTestOptions {
  return {
    key: str(options?.key),
    refreshIdentity: bool(options?.refreshIdentity),
    probeWithoutPrinting: bool(options?.probeWithoutPrinting),
    noBarcode: bool(options?.noBarcode),
    barcodeData: str(options?.barcodeData),
    noVerificationId: bool(options?.noVerificationId),
    timeoutMs: int(options?.timeoutMs),
  };
}

export interface NativeAutoDetectOptions {
  subnetCidr: string;
  endpoints: string[];
  port: number;
  concurrency: number;
  connectTimeoutMs: number;
  responseTimeoutMs: number;
  leaveUnknownUnprobed: number;
  statusTimeoutMs: number;
  identityTimeoutMs: number;
  completionTimeoutMs: number;
}

export function marshalAutoDetectOptions(
  options: AutoDetectOptions | undefined
): NativeAutoDetectOptions {
  return {
    subnetCidr: str(options?.subnetCidr),
    endpoints: options?.endpoints === undefined ? [] : [...options.endpoints],
    port: int(options?.port),
    concurrency: int(options?.concurrency),
    connectTimeoutMs: int(options?.connectTimeoutMs),
    responseTimeoutMs: int(options?.responseTimeoutMs),
    leaveUnknownUnprobed: bool(options?.leaveUnknownUnprobed),
    statusTimeoutMs: int(options?.statusTimeoutMs),
    identityTimeoutMs: int(options?.identityTimeoutMs),
    completionTimeoutMs: int(options?.completionTimeoutMs),
  };
}

export interface NativeDiscoverOptions {
  subnetCidr: string;
  port: number;
  concurrency: number;
  connectTimeoutMs: number;
  responseTimeoutMs: number;
  noBackchannelProbe: number;
}

export function marshalDiscoverOptions(
  options: DiscoverOptions | undefined
): NativeDiscoverOptions {
  return {
    subnetCidr: str(options?.subnetCidr),
    port: int(options?.port),
    concurrency: int(options?.concurrency),
    connectTimeoutMs: int(options?.connectTimeoutMs),
    responseTimeoutMs: int(options?.responseTimeoutMs),
    noBackchannelProbe: bool(options?.noBackchannelProbe),
  };
}

// --- outbound: the receipt DSL ------------------------------------------------------------------

export interface NativeRenderOptions {
  widthDots: number;
  cutClearanceDots: number;
  maxRowsPerBand: number;
  locale: string;
  currency: string;
  timeZone: string;
}

export function marshalRenderOptions(options: RenderOptions | undefined): NativeRenderOptions {
  return {
    widthDots: int(options?.widthDots),
    cutClearanceDots: int(options?.cutClearanceDots),
    maxRowsPerBand: int(options?.maxRowsPerBand),
    // "" is the ABI's own "defer to the document's meta", which is why an omitted
    // override is not turned into an empty locale that overrides it with nothing.
    locale: str(options?.locale),
    currency: str(options?.currency),
    timeZone: str(options?.timeZone),
  };
}

/**
 * A JSON string stays exactly as it is; anything else goes through `JSON.stringify`, so an
 * object built in JavaScript and a stored `.json` template reach the identical parser in
 * the core. `undefined` and `null` become "", which the native side turns back into the
 * NULL that means "this document carries its own content".
 */
export function marshalJsonDocument(
  value: JsonDocument | null | undefined,
  what: string
): string {
  if (value === undefined || value === null) return '';
  if (typeof value === 'string') return value;
  let text: string;
  try {
    text = JSON.stringify(value);
  } catch (error) {
    throw new PayloadError(`the ${what} is not representable as JSON: ${String(error)}`);
  }
  if (text === undefined) {
    throw new PayloadError(`the ${what} is not representable as JSON`);
  }
  return text;
}

// --- outbound: payloads -----------------------------------------------------------------------

export interface NativeDocumentOp {
  kind: number;
  /** null is a bare LF for `line`, and is never produced for the other kinds. */
  text: string | null;
  value: number;
}

export type NativePayload =
  | {
      kind: 0;
      pixels: ArrayBuffer;
      width: number;
      height: number;
      strideBytes: number;
      binarization: number;
      threshold: number;
      maxRowsPerBand: number;
    }
  | { kind: 1; ops: NativeDocumentOp[]; codePage: number }
  | { kind: 2; bytes: ArrayBuffer };

export class PayloadError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'PayloadError';
  }
}

export function marshalDocumentOp(op: DocumentOp): NativeDocumentOp {
  switch (op.kind) {
    case 'text':
      return { kind: OpKind.text, text: op.text, value: 0 };
    case 'line':
      return { kind: OpKind.line, text: op.text === undefined ? null : op.text, value: 0 };
    case 'align':
      return { kind: OpKind.align, text: null, value: Alignment[op.value] };
    case 'bold':
      return { kind: OpKind.bold, text: null, value: op.value ? 1 : 0 };
    case 'feed':
      if (!Number.isInteger(op.lines) || op.lines < 1 || op.lines > 255) {
        throw new PayloadError(`feed: lines must be an integer 1..255, got ${op.lines}`);
      }
      return { kind: OpKind.feed, text: null, value: op.lines };
    default: {
      const exhaustive: never = op;
      throw new PayloadError(`unknown document op ${JSON.stringify(exhaustive)}`);
    }
  }
}

export function marshalPayload(payload: Payload): NativePayload {
  switch (payload.kind) {
    case 'raster': {
      const stride = int(payload.strideBytes);
      const minimumStride = payload.width * 4;
      if (payload.width <= 0 || payload.height <= 0) {
        throw new PayloadError('raster: width and height must both be positive');
      }
      if (stride !== 0 && stride < minimumStride) {
        throw new PayloadError(
          `raster: strideBytes ${stride} is narrower than ${minimumStride} bytes of RGBA8`
        );
      }
      const rowBytes = stride === 0 ? minimumStride : stride;
      const needed = rowBytes * payload.height;
      if (payload.pixels.byteLength < needed) {
        throw new PayloadError(
          `raster: pixels holds ${payload.pixels.byteLength} bytes, ${needed} are needed for ` +
            `${payload.width}x${payload.height} RGBA8`
        );
      }
      return {
        kind: 0,
        pixels: payload.pixels,
        width: payload.width,
        height: payload.height,
        strideBytes: stride,
        binarization: Binarization[payload.binarization ?? 'fixedThreshold'],
        threshold: int(payload.threshold),
        maxRowsPerBand: int(payload.maxRowsPerBand),
      };
    }
    case 'document':
      return {
        kind: 1,
        ops: payload.ops.map(marshalDocumentOp),
        codePage: CodePage[payload.codePage ?? 'pc437'],
      };
    case 'raw':
      if (payload.bytes.byteLength === 0) {
        throw new PayloadError('raw: refusing to submit an empty payload');
      }
      return { kind: 2, bytes: payload.bytes };
    default: {
      const exhaustive: never = payload;
      throw new PayloadError(`unknown payload ${JSON.stringify(exhaustive)}`);
    }
  }
}

// --- inbound: results and structs -----------------------------------------------------------------

export interface NativeJobResult {
  outcome: number;
  confidence: number;
  reason: number;
  grade: number;
  authority: number;
  method: string;
}

export function unmarshalJobResult(raw: NativeJobResult): JobResult {
  const outcome = jobOutcomeFromNative(raw.outcome);
  const confidence = confidenceLevelFromNative(raw.confidence);
  const grade = confidenceGradeFromNative(raw.grade);
  const authority = completionAuthorityFromNative(raw.authority);
  const method = raw.method;
  switch (outcome) {
    case 'done':
      return { outcome, confidence, grade, authority, method };
    case 'failed':
    case 'unknown':
      return {
        outcome,
        confidence,
        reason: failureReasonFromNative(raw.reason),
        grade,
        authority,
        method,
      };
    default: {
      const exhaustive: never = outcome;
      throw new Error(`unreachable job outcome ${String(exhaustive)}`);
    }
  }
}

export interface NativeJobEvent {
  state: number;
  confidence: number;
  hasReason: number;
  reason: number;
  monotonicMs: number;
}

export function unmarshalJobEvent(raw: NativeJobEvent): JobEvent {
  const base = {
    state: jobStateFromNative(raw.state),
    confidence: confidenceLevelFromNative(raw.confidence),
    monotonicMs: raw.monotonicMs,
  };
  return raw.hasReason === 0
    ? base
    : { ...base, reason: failureReasonFromNative(raw.reason) };
}

export interface NativeDeviceStatus {
  connected: number;
  observed: number;
  online: number;
  coverOpen: number;
  paperOut: number;
  paperNearEnd: number;
  cutterError: number;
  unrecoverableError: number;
  recoverableError: number;
}

export function unmarshalDeviceStatus(raw: NativeDeviceStatus): DeviceStatus {
  return {
    connected: raw.connected === 1,
    observed: raw.observed === 1,
    online: tristateFromNative(raw.online),
    coverOpen: tristateFromNative(raw.coverOpen),
    paperOut: tristateFromNative(raw.paperOut),
    paperNearEnd: tristateFromNative(raw.paperNearEnd),
    cutterError: tristateFromNative(raw.cutterError),
    unrecoverableError: tristateFromNative(raw.unrecoverableError),
    recoverableError: tristateFromNative(raw.recoverableError),
  };
}

export interface NativeDrawerCapabilities {
  present: number;
  standard: number;
  voltage: number;
  maxCurrentMa: number;
  channelCount: number;
  sensorPin: number;
  method: number;
  defaultPulseMs: number;
  maxPulseMs: number;
  cooldownMs: number;
  canKickDuringPrint: number;
  statusAvailable: number;
  statusMethod: number;
  sharedBetweenDrawers: number;
  sharedWithBuzzer: number;
  electricalProvenance: number;
  commandsProvenance: number;
  kickable: number;
}

export function unmarshalDrawerCapabilities(
  raw: NativeDrawerCapabilities
): DrawerCapabilities {
  return {
    present: raw.present === 1,
    standard: drawerPortStandardFromNative(raw.standard),
    voltage: raw.voltage,
    maxCurrentMa: raw.maxCurrentMa,
    channelCount: raw.channelCount,
    sensorPin: raw.sensorPin,
    method: drawerKickMethodFromNative(raw.method),
    defaultPulseMs: raw.defaultPulseMs,
    maxPulseMs: raw.maxPulseMs,
    cooldownMs: raw.cooldownMs,
    canKickDuringPrint: raw.canKickDuringPrint === 1,
    statusAvailable: raw.statusAvailable === 1,
    statusMethod: drawerStatusMethodFromNative(raw.statusMethod),
    sharedBetweenDrawers: raw.sharedBetweenDrawers === 1,
    sharedWithBuzzer: raw.sharedWithBuzzer === 1,
    electricalProvenance: provenanceFromNative(raw.electricalProvenance),
    commandsProvenance: provenanceFromNative(raw.commandsProvenance),
    kickable: raw.kickable === 1,
  };
}

export interface NativeDrawerResult {
  state: number;
  previousState: number;
  channel: number;
  pulseMs: number;
  elapsedMs: number;
}

export function unmarshalDrawerResult(raw: NativeDrawerResult): DrawerResult {
  return {
    state: drawerStateFromNative(raw.state),
    previousState: drawerStateFromNative(raw.previousState),
    channel: raw.channel,
    pulseMs: raw.pulseMs,
    elapsedMs: raw.elapsedMs,
  };
}

export interface NativeDrawerReading {
  available: number;
  answered: number;
  pinHigh: number;
  needsCalibration: number;
  state: number;
}

export function unmarshalDrawerReading(raw: NativeDrawerReading): DrawerReading {
  return {
    available: raw.available === 1,
    answered: raw.answered === 1,
    pinHigh: tristateFromNative(raw.pinHigh),
    needsCalibration: raw.needsCalibration === 1,
    state: drawerStateFromNative(raw.state),
  };
}

export interface NativeDetectionSummary {
  endpoint: string;
  vendor: string;
  model: string;
  firmware: string;
  serial: string;
  identityTrusted: number;
  confidencePercent: number;
  impersonationSuspected: number;
  identityFresh: number;
  profileId: string;
  selection: number;
  nominalPaperMm: number;
  printableWidthDots: number;
  charsPerLine: number;
  dpi: number;
  completion: number;
  gradeCeiling: number;
  authority: number;
  method: string;
  completionProvenance: number;
  drawerPresent: number;
  drawerKickable: number;
  drawerStandard: number;
  drawerVoltage: number;
  drawerElectricalProvenance: number;
  drawerCommandsProvenance: number;
  provenanceSummary: string;
  degradations: string[];
}

export function unmarshalDetectionSummary(raw: NativeDetectionSummary): DetectionSummary {
  return {
    endpoint: raw.endpoint,
    vendor: raw.vendor,
    model: raw.model,
    firmware: raw.firmware,
    serial: raw.serial,
    identityTrusted: raw.identityTrusted === 1,
    confidencePercent: raw.confidencePercent,
    impersonationSuspected: raw.impersonationSuspected === 1,
    identityFresh: raw.identityFresh === 1,
    profileId: raw.profileId,
    selection: profileSelectionFromNative(raw.selection),
    nominalPaperMm: raw.nominalPaperMm,
    printableWidthDots: raw.printableWidthDots,
    charsPerLine: raw.charsPerLine,
    dpi: raw.dpi,
    completion: completionMechanismFromNative(raw.completion),
    gradeCeiling: confidenceGradeFromNative(raw.gradeCeiling),
    authority: completionAuthorityFromNative(raw.authority),
    method: raw.method,
    completionProvenance: provenanceFromNative(raw.completionProvenance),
    drawerPresent: raw.drawerPresent === 1,
    drawerKickable: raw.drawerKickable === 1,
    drawerStandard: drawerPortStandardFromNative(raw.drawerStandard),
    drawerVoltage: raw.drawerVoltage,
    drawerElectricalProvenance: provenanceFromNative(raw.drawerElectricalProvenance),
    drawerCommandsProvenance: provenanceFromNative(raw.drawerCommandsProvenance),
    provenanceSummary: raw.provenanceSummary,
    degradations: raw.degradations,
  };
}

export interface NativeDetectedPrinter {
  endpoint: string;
  host: string;
  port: number;
  status: number;
  portOpen: number;
  fromCache: number;
  dleEotHex: string;
  summary: NativeDetectionSummary;
}

export function unmarshalDetectedPrinter(raw: NativeDetectedPrinter): DetectedPrinter {
  return {
    endpoint: raw.endpoint,
    host: raw.host,
    port: raw.port,
    status: detectionStatusFromNative(raw.status),
    portOpen: raw.portOpen === 1,
    fromCache: raw.fromCache === 1,
    dleEotHex: raw.dleEotHex,
    summary: unmarshalDetectionSummary(raw.summary),
  };
}

export interface NativeDiscoveredDevice {
  ip: string;
  port: number;
  port9100Open: number;
  dleEotHex: string;
}

export function unmarshalDiscoveredDevice(raw: NativeDiscoveredDevice): DiscoveredDevice {
  return {
    ip: raw.ip,
    port: raw.port,
    port9100Open: raw.port9100Open === 1,
    dleEotHex: raw.dleEotHex,
  };
}

// --- inbound: the receipt DSL ---------------------------------------------------------------------

export interface NativeReportEntry {
  kind: number;
  block: string;
  requested: string;
  delivered: string;
  path: number;
  note: string;
}

export function unmarshalReportEntry(raw: NativeReportEntry): ReportEntry {
  return {
    kind: reportKindFromNative(raw.kind),
    block: raw.block,
    requested: raw.requested,
    delivered: raw.delivered,
    path: renderPathFromNative(raw.path),
    note: raw.note,
  };
}

export function unmarshalReport(raw: readonly NativeReportEntry[] | undefined): ReportEntry[] {
  return raw === undefined ? [] : raw.map(unmarshalReportEntry);
}

/** One line, for a log, an error message or a test failure. */
export function describeReportEntry(entry: ReportEntry): string {
  const because = entry.note.length === 0 ? '' : ` - ${entry.note}`;
  return (
    `${entry.block}  ${entry.kind}: requested "${entry.requested}", ` +
    `delivered "${entry.delivered}" [${entry.path}]${because}`
  );
}

export interface NativeRenderResult {
  /** 1 when bytes were produced. 0 means the report says why, and nothing printed. */
  ok: number;
  bytes: ArrayBuffer;
  codePage: number;
  hasCut: number;
  cut: number;
  topFeedDots: number;
  bottomFeedDots: number;
  report: NativeReportEntry[];
  /** pd_last_error, read on the thread that made the call. "" on the success path. */
  error: string;
}

export function unmarshalRenderedDocument(raw: NativeRenderResult): RenderedDocument {
  return {
    bytes: raw.bytes,
    codePage: codePageFromNative(raw.codePage),
    meta: {
      // has_cut = 0 is "the document asked for no particular cut", which is not the same
      // as asking for `profile`; null keeps the two distinguishable.
      cut: raw.hasCut === 0 ? null : cutSettingFromNative(raw.cut),
      topFeedDots: raw.topFeedDots,
      bottomFeedDots: raw.bottomFeedDots,
    },
    report: unmarshalReport(raw.report),
  };
}

export interface NativePrintDocumentResult {
  /** 0 when nothing was submitted. */
  job: number;
  report: NativeReportEntry[];
  error: string;
}
