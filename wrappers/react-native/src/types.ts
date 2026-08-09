/**
 * The value types an app sees. Everything here is plain data — no handles, no ints that
 * mean an enum, no booleans standing in for a tri-state.
 */

import type {
  Alignment,
  Binarization,
  CodePage,
  CommandLanguage,
  CompletionAuthority,
  CompletionMechanism,
  ConfidenceGrade,
  ConfidenceLevel,
  CutSetting,
  DetectionStatus,
  DeviceEvent,
  DrainOrder,
  DrawerKickMethod,
  DrawerPortStandard,
  DrawerState,
  DrawerStatusMethod,
  FailureReason,
  JobState,
  PreflightMode,
  ProfileSelection,
  Provenance,
  Tristate,
} from './enums.ts';

// --- driver configuration ---------------------------------------------------------------

export interface DriverConfig {
  /**
   * Where the job journal lives. Omitted (or empty) means in-memory: no journal, no
   * crash recovery, no persisted instance nonce. A POS app should always pass one — on
   * RN that is typically `RNFS.DocumentDirectoryPath + '/printerdriver'` or the
   * equivalent from `expo-file-system`.
   */
  storageDirectory?: string;
  /** Tests only. Leaving it false keeps the durability rule. */
  fsyncDisabled?: boolean;
  /** ABI-level diagnostics. Delivered on the JS thread like every other callback. */
  onLog?: (message: string) => void;
}

export interface TcpPrinterConfig {
  host: string;
  /** Defaults to 9100. */
  port?: number;
  /** Stable id for this printer; derived from the endpoint when omitted. */
  printerId?: string;
  /** 384 | 504 | 576 are the deployed widths. Defaults to 576. */
  widthDots?: number;
  /** One of `driver.profileIds()`. Defaults to "generic". */
  profileId?: string;
  /** Defaults to 3000 ms. */
  connectTimeoutMs?: number;
}

// --- job options --------------------------------------------------------------------------

export interface JobOptions {
  /**
   * Idempotency key — a caller-supplied stable id (order/ticket UUID). Omitted means the
   * SDK generates one: feedback still works, but there is no dedupe protection across
   * restarts, so fleet/POS apps should always pass one.
   */
  key?: string;
  /** Defaults to `profile` — whatever this printer's cutter does. */
  cut?: CutSetting;
  openDrawer?: boolean;
  /** `strict` (default) refuses on cover-open/paper-out; `skip` does not. */
  preflight?: PreflightMode;
  /** Completion-wait budget. 0 or omitted uses the profile's own. */
  timeoutMs?: number;
  /** Fed before the first content byte. */
  topFeedDots?: number;
  /**
   * TOTAL clearance between the last content and the cut. The engine feeds
   * max(profile blade clearance, this), so it can only add whitespace and can never
   * reintroduce a clipped trailing QR.
   */
  bottomFeedDots?: number;
  /**
   * Print the `V:` trailer line and QR carrying this job's verification identifier
   * (docs/api.md §14). Defaults to true. The token is journaled and `jobByToken`
   * resolves it either way.
   */
  printVerificationId?: boolean;
}

export interface ReprintOptions extends JobOptions {
  /**
   * Print the REPRINT / POSSIBLE DUPLICATE banner. Defaults to true. Disabling it is a
   * per-call, deliberate act for a receipt where the banner is inappropriate; a kitchen
   * ticket should never disable it, because the banner is what lets staff bin the
   * duplicate instead of cooking it twice.
   */
  banner?: boolean;
}

// --- payloads -------------------------------------------------------------------------------

/** Tier 2 document operations. Build these with the helpers in src/payload.ts. */
export type DocumentOp =
  | { readonly kind: 'text'; readonly text: string }
  | { readonly kind: 'line'; readonly text?: string }
  | { readonly kind: 'align'; readonly value: Alignment }
  | { readonly kind: 'bold'; readonly value: boolean }
  | { readonly kind: 'feed'; readonly lines: number };

export interface RasterPayload {
  readonly kind: 'raster';
  /** Tightly packed or `strideBytes`-strided RGBA8. Crosses the bridge without a copy. */
  readonly pixels: ArrayBuffer;
  readonly width: number;
  readonly height: number;
  /** 0 (the default) for tightly packed rows. */
  readonly strideBytes?: number;
  readonly binarization?: Binarization;
  /** Only read for `fixedThreshold`; 0 means 128. */
  readonly threshold?: number;
  /** 0 means 1024, the Epson tall-image split. */
  readonly maxRowsPerBand?: number;
}

export interface DocumentPayload {
  readonly kind: 'document';
  readonly ops: readonly DocumentOp[];
  readonly codePage?: CodePage;
}

export interface RawPayload {
  readonly kind: 'raw';
  /**
   * Passed through verbatim. Must not embed its own cuts or realtime status tricks: the
   * core owns job termination and its trailing fence assumes that.
   */
  readonly bytes: ArrayBuffer;
}

export type Payload = RasterPayload | DocumentPayload | RawPayload;

// --- feedback ----------------------------------------------------------------------------------

export interface JobEvent {
  readonly state: JobState;
  readonly confidence: ConfidenceLevel;
  /** Present only on failure transitions. */
  readonly reason?: FailureReason;
  /** A steady clock, because the wall clock may step and job timing must not. */
  readonly monotonicMs: number;
}

/**
 * The terminal answer, as a discriminated union. There is no `success` boolean anywhere
 * in this package (docs/api.md §1.4) — an exhaustive `switch (result.outcome)` is the
 * point, and TypeScript's `never` check makes forgetting `'unknown'` a compile error.
 *
 * `confidence` is carried on all three outcomes, not only on `done`: on `failed` and
 * `unknown` it records how far up the evidence ladder the job got before it stopped,
 * which is what an operator needs to decide about a reprint. `grade`, `authority` and
 * `method` say what the claim is made of and who made it.
 */
export type JobResult =
  | {
      readonly outcome: 'done';
      readonly confidence: ConfidenceLevel;
      readonly grade: ConfidenceGrade;
      readonly authority: CompletionAuthority;
      /** The actual command, e.g. "GS(H) fn48". "none" when nothing was confirmed. */
      readonly method: string;
    }
  | {
      readonly outcome: 'failed';
      readonly confidence: ConfidenceLevel;
      readonly reason: FailureReason;
      readonly grade: ConfidenceGrade;
      readonly authority: CompletionAuthority;
      readonly method: string;
    }
  | {
      readonly outcome: 'unknown';
      readonly confidence: ConfidenceLevel;
      /** Why the job stopped short of an acknowledgement, when that much is known. */
      readonly reason: FailureReason;
      readonly grade: ConfidenceGrade;
      readonly authority: CompletionAuthority;
      readonly method: string;
    };

/**
 * Last known device state — never a live query, so it cannot block behind a print.
 * `observed` is false until a status frame has actually been decoded: a snapshot that has
 * never heard from the device says so rather than reporting healthy.
 */
export interface DeviceStatus {
  readonly connected: boolean;
  readonly observed: boolean;
  readonly online: Tristate;
  readonly coverOpen: Tristate;
  readonly paperOut: Tristate;
  readonly paperNearEnd: Tristate;
  readonly cutterError: Tristate;
  readonly unrecoverableError: Tristate;
  readonly recoverableError: Tristate;
}

export interface DeviceEventMessage {
  readonly printerId: string;
  readonly event: DeviceEvent;
}

// --- cash drawer ---------------------------------------------------------------------------------

export interface DrawerCapabilities {
  readonly present: boolean;
  readonly standard: DrawerPortStandard;
  /** 0 where the manufacturer does not document it, which is not the same as "low". */
  readonly voltage: number;
  readonly maxCurrentMa: number;
  readonly channelCount: number;
  /** 3 on the Epson arrangement and 6 on Star's. */
  readonly sensorPin: number;
  readonly method: DrawerKickMethod;
  readonly defaultPulseMs: number;
  readonly maxPulseMs: number;
  readonly cooldownMs: number;
  readonly canKickDuringPrint: boolean;
  readonly statusAvailable: boolean;
  readonly statusMethod: DrawerStatusMethod;
  /** Two outputs, ONE switch input: only "some attached drawer is open" is readable. */
  readonly sharedBetweenDrawers: boolean;
  /** Epson's documented conflict: with the buzzer enabled the pulse sounds the buzzer. */
  readonly sharedWithBuzzer: boolean;
  readonly electricalProvenance: Provenance;
  readonly commandsProvenance: Provenance;
  /** A caller that reads nothing else should read this. */
  readonly kickable: boolean;
}

export interface DrawerRequest {
  /** 1 for drive 1 (Epson pin 2), 2 for drive 2 (pin 5). 0 uses the profile default. */
  readonly channel?: number;
  /** 0 uses the profile default. */
  readonly pulseMs?: number;
}

export interface DrawerResult {
  readonly state: DrawerState;
  readonly previousState: DrawerState;
  readonly channel: number;
  /** 0 when no pulse was emitted at all. */
  readonly pulseMs: number;
  /** The "sensor changed 143 ms after kick" number. 0 when there was nothing to wait for. */
  readonly elapsedMs: number;
}

export interface DrawerReading {
  readonly available: boolean;
  readonly answered: boolean;
  readonly pinHigh: Tristate;
  /** While true, `state` stays `unknown` however clear the level is. */
  readonly needsCalibration: boolean;
  readonly state: DrawerState;
}

// --- detection ------------------------------------------------------------------------------------

export interface DetectionSummary {
  readonly endpoint: string;
  readonly vendor: string;
  readonly model: string;
  readonly firmware: string;
  readonly serial: string;
  /** True only when a signal independent of GS I agrees with GS I. */
  readonly identityTrusted: boolean;
  readonly confidencePercent: number;
  readonly impersonationSuspected: boolean;
  readonly identityFresh: boolean;
  readonly profileId: string;
  readonly selection: ProfileSelection;
  /** Roll width and raster width are separate facts: 112 mm media prints 104 mm. */
  readonly nominalPaperMm: number;
  readonly printableWidthDots: number;
  readonly charsPerLine: number;
  readonly dpi: number;
  readonly completion: CompletionMechanism;
  /** The best grade a job on this printer can ever claim. */
  readonly gradeCeiling: ConfidenceGrade;
  readonly authority: CompletionAuthority;
  readonly method: string;
  readonly completionProvenance: Provenance;
  readonly drawerPresent: boolean;
  readonly drawerKickable: boolean;
  readonly drawerStandard: DrawerPortStandard;
  readonly drawerVoltage: number;
  readonly drawerElectricalProvenance: Provenance;
  readonly drawerCommandsProvenance: Provenance;
  /** One line, for a table row. */
  readonly provenanceSummary: string;
  /** Everything requested and not delivered, in the words it is printed in. */
  readonly degradations: readonly string[];
}

export interface DetectedPrinter {
  readonly endpoint: string;
  readonly host: string;
  readonly port: number;
  readonly status: DetectionStatus;
  readonly portOpen: boolean;
  readonly fromCache: boolean;
  /** DLE EOT 1's answer as uppercase hex; "" when the port said nothing. */
  readonly dleEotHex: string;
  readonly summary: DetectionSummary;
}

export interface DiscoveredDevice {
  readonly ip: string;
  readonly port: number;
  readonly port9100Open: boolean;
  readonly dleEotHex: string;
}

export interface AutoDetectOptions {
  /** Omitted means the local /24. Ignored when `endpoints` is set. */
  subnetCidr?: string;
  /** "host" or "host:port". The only path that reports an unreachable address. */
  endpoints?: readonly string[];
  port?: number;
  concurrency?: number;
  connectTimeoutMs?: number;
  responseTimeoutMs?: number;
  /** Cached findings still apply; anything untouched comes back `unverified`. */
  leaveUnknownUnprobed?: boolean;
  statusTimeoutMs?: number;
  identityTimeoutMs?: number;
  completionTimeoutMs?: number;
}

export interface DiscoverOptions {
  subnetCidr?: string;
  port?: number;
  concurrency?: number;
  connectTimeoutMs?: number;
  responseTimeoutMs?: number;
  /** Turns the sweep into a pure port scan: not one byte is written. */
  noBackchannelProbe?: boolean;
}

export interface SelfTestOptions {
  /** Omitted means "selftest-<unix ms>". A real key: running it twice does not print twice. */
  key?: string;
  refreshIdentity?: boolean;
  /** Only meaningful with `refreshIdentity`. */
  probeWithoutPrinting?: boolean;
  noBarcode?: boolean;
  /** Omitted means "PD-SELFTEST". */
  barcodeData?: string;
  noVerificationId?: boolean;
  timeoutMs?: number;
}

// --- print queue -------------------------------------------------------------------------------------

export interface QueuePolicy {
  /**
   * Park jobs while the printer is known to be offline, coverless or out of paper instead
   * of failing them one at a time. False makes the queue a pure serializer.
   */
  holdWhileOffline?: boolean;
  /** Shelf life for a held job. 0 means never. A kitchen ticket must not print late. */
  defaultTtlMs?: number;
  /** Held jobs per printer. 0 is unlimited; the C++ default is 64 and is what to copy. */
  maxDepth?: number;
  drainOrder?: DrainOrder;
}

export interface QueueOptions {
  key?: string;
  /** 0 uses the policy default. */
  ttlMs?: number;
  /** Orders the waiting set only; in flight is never preempted. */
  priority?: number;
  cut?: CutSetting;
  openDrawer?: boolean;
  preflight?: PreflightMode;
  timeoutMs?: number;
}

// --- the language a printer is driven in -----------------------------------------------------------

export interface PrinterCapabilities {
  readonly id: string;
  readonly widthDots: number;
  readonly completion: CompletionMechanism;
  readonly completionProvenance: Provenance;
  readonly language: CommandLanguage;
}
