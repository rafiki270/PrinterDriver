/**
 * The React Native codegen spec for the PrinterDriver TurboModule.
 *
 * WHAT THIS FILE IS
 *   One method per public `pd_*` function in capi/include/printerdriver/pd.h, named by a
 *   single mechanical rule: drop the `pd_` prefix and camelCase the rest.
 *   `pd_add_printer_tcp` -> `addPrinterTcp`, `pd_job_await` -> `jobAwait`,
 *   `pd_queue_is_blocked` -> `queueIsBlocked`. Nothing is renamed for taste, because
 *   test/parity.test.ts derives the expected name from pd.h with that same rule and fails
 *   when one is missing (docs/api.md §17). The friendly API — `PrinterDriver`, `Printer`,
 *   `PrintJob`, `PrintQueue` — is built on top of this, in the other files of src/.
 *
 * WHY THE SHAPES LOOK LIKE THIS
 *   - Handles (`pd_driver*`, `pd_printer*`, `pd_job*`, `pd_queue*`) cross as `Int32`
 *     tokens minted by a table in the native module, never as raw pointers. 0 is the null
 *     handle, which is how `pd_find_job` reports "no such key" without an exception.
 *   - Every `pd_*` call that BLOCKS returns a `Promise`. The native module runs those on
 *     its own worker thread and never on the JS thread. That is not an ergonomic
 *     preference: the custom-transport and custom-registration callbacks below have to
 *     run *on* the JS thread, so a JS thread parked inside a blocking ABI call would
 *     deadlock the core worker waiting for it. Keeping every blocking call off the JS
 *     thread is what makes the callback hop safe. See cpp/PrinterDriverModule.h.
 *   - Structs cross as plain objects with the C enum values as numbers. src/enums.ts turns
 *     those into closed string unions at the edge; nothing above src/native.ts sees an int.
 *   - Raster pixels cross as an `ArrayBuffer` (typed `UnsafeObject`, because codegen has
 *     no ArrayBuffer type). The native side reads its data pointer directly and hands it
 *     to `pd_print` without a copy — the zero-copy path docs/platforms.md asks for. Base64
 *     exists only as the old-architecture fallback and is not offered here.
 *
 * EVENTS AND JS-SUPPLIED CALLBACKS: ONE SINK, TWO DIRECTIONS
 *   `setEventSink` installs a single JS function. Everything the native side has to tell
 *   JavaScript travels through it as `(kind, payload)`:
 *
 *     kind                       payload
 *     'job'                      { job, state, confidence, hasReason, reason, monotonicMs }
 *     'device'                   { printer, event }
 *     'detected'                 { requestId, printer, completed, total }   (pd_auto_detect)
 *     'discovered'               { requestId, device, completed, total }    (pd_discover)
 *     'transport.connect'        { requestId, transport }
 *     'transport.write'          { requestId, transport, data: ArrayBuffer }
 *     'transport.close'          { requestId, transport }
 *     'completion.fenceBytes'    { requestId, id, jobToken, cap }
 *     'completion.match'         { requestId, id, data: ArrayBuffer }
 *     'probe.classify'           { requestId, id, response: ArrayBuffer }
 *     'block.render'             { requestId, kind, blockJson, profileJson, cap }
 *     'formatter.format'         { requestId, name, value, args, locale, cap }
 *     'drawer.kickBytes'         { requestId, id, channel, pulseMs, cap }
 *     'drawer.statusRequest'     { requestId, id, cap }
 *     'drawer.statusParse'       { requestId, id, response: ArrayBuffer }
 *
 *   Everything carrying a `requestId` is a QUESTION: a core thread is blocked waiting for
 *   the answer, and JavaScript must call `respond(requestId, value)` exactly once. The
 *   native side waits with a deadline and treats a missed deadline as the registration's
 *   failure (a short write, a `notMine` match) rather than hanging a printer forever.
 *   `'job'` and `'device'` carry no requestId: they are one-way notifications and nothing
 *   waits for them.
 */

import type { TurboModule } from 'react-native';
import { TurboModuleRegistry } from 'react-native';
import type { Int32, UnsafeObject } from 'react-native/Libraries/Types/CodegenTypes';

import { methodNameForAbiFunction, nativeMethodNames } from './methodNames.ts';

export { methodNameForAbiFunction, nativeMethodNames };

export interface Spec extends TurboModule {
  // --- module plumbing (not part of the C ABI) -----------------------------------------

  /** Install the one JS function every native->JS message travels through. */
  setEventSink(sink: (kind: string, payload: UnsafeObject) => void): void;
  /** Answer a sink message that carried a `requestId`. Exactly once per requestId. */
  respond(requestId: Int32, value: UnsafeObject): void;

  // --- driver (pd_create, pd_destroy, pd_last_error, pd_profile_ids) --------------------

  create(config: UnsafeObject): Int32;
  /** Blocks: stops every worker and waits for in-flight jobs to terminate. */
  destroy(driver: Int32): Promise<void>;
  lastError(driver: Int32): string;
  profileIds(): string[];
  instanceNonce(driver: Int32): string;
  localSubnet(driver: Int32): string;

  // --- printers -------------------------------------------------------------------------

  addPrinterTcp(driver: Int32, config: UnsafeObject): Int32;
  /**
   * `transport` is a JS-side transport id. The native module builds a `pd_transport_vtable`
   * whose ctx is that id and routes connect/write/close through the event sink.
   */
  addPrinterCustom(
    driver: Int32,
    transport: Int32,
    description: string,
    profileId: string,
    widthDots: Int32
  ): Int32;
  transportFeedBytes(printer: Int32, data: UnsafeObject): boolean;
  transportLinkDropped(printer: Int32, message: string): boolean;
  printerId(printer: Int32): string;
  printerWidthDots(printer: Int32): Int32;
  printerCompletion(printer: Int32): Int32;
  printerCompletionProvenance(printer: Int32): Int32;
  printerLanguage(printer: Int32): Int32;
  printerStatus(driver: Int32, printer: Int32): UnsafeObject;
  /** Blocks: queues a status round trip behind any active job. */
  printerRefreshStatus(driver: Int32, printer: Int32, timeoutMs: Int32): Promise<UnsafeObject>;
  openCashDrawer(driver: Int32, printer: Int32): void;
  /** Blocks until the queue is empty and the active job is terminal. */
  printerDrain(driver: Int32, printer: Int32): Promise<void>;
  subscribeDevice(driver: Int32, printer: Int32): void;

  // --- jobs -----------------------------------------------------------------------------

  print(driver: Int32, printer: Int32, payload: UnsafeObject, options: UnsafeObject): Int32;
  forceReprint(driver: Int32, printer: Int32, key: string, options: UnsafeObject): Int32;
  forceReprintOpts(driver: Int32, printer: Int32, key: string, options: UnsafeObject): Int32;
  findJob(driver: Int32, key: string): Int32;
  jobByToken(driver: Int32, token: string): Int32;
  jobId(job: Int32): string;
  jobKey(job: Int32): string;
  jobPrintToken(job: Int32): string;
  jobCutToken(job: Int32): string;
  jobAttempt(job: Int32): Int32;
  jobCurrentState(job: Int32): Int32;
  jobConfidence(job: Int32): Int32;
  jobIsTerminal(job: Int32): boolean;
  subscribeJob(driver: Int32, job: Int32): void;
  /** Blocks: waits for the terminal result. Resolves null on timeout. */
  jobAwait(driver: Int32, job: Int32, timeoutMs: Int32): Promise<UnsafeObject | null>;

  // --- enum names (the core's own spelling, for logs and support tickets) ---------------

  jobStateName(value: Int32): string;
  confidenceLevelName(value: Int32): string;
  deviceEventName(value: Int32): string;
  failureReasonName(value: Int32): string;
  jobOutcomeName(value: Int32): string;
  confidenceGradeName(value: Int32): string;
  confidenceGradeLetter(value: Int32): string;
  completionAuthorityName(value: Int32): string;
  provenanceName(value: Int32): string;
  commandLanguageName(value: Int32): string;
  payloadKindName(value: Int32): string;
  completionMechanismName(value: Int32): string;
  cutVariantName(value: Int32): string;
  drainOrderName(value: Int32): string;
  drawerStateName(value: Int32): string;
  drawerPortStandardName(value: Int32): string;
  drawerKickMethodName(value: Int32): string;
  drawerStatusMethodName(value: Int32): string;
  profileSelectionName(value: Int32): string;
  detectionStatusName(value: Int32): string;
  matchKindName(value: Int32): string;
  /** The non-contiguous code page enum, by member index. */
  codePageAt(index: Int32): Int32;

  // --- print queue addon -----------------------------------------------------------------

  queueCreate(driver: Int32, policy: UnsafeObject): Int32;
  /** Blocks: stops the queue thread. Held jobs stay held and stay non-terminal. */
  queueDestroy(queue: Int32): Promise<void>;
  queueEnqueue(queue: Int32, printer: Int32, payload: UnsafeObject, options: UnsafeObject): Int32;
  queuePause(queue: Int32, printerId: string): void;
  queueResume(queue: Int32, printerId: string): void;
  queueIsPaused(queue: Int32, printerId: string): boolean;
  queueIsBlocked(queue: Int32, printerId: string): boolean;
  queueUnblock(queue: Int32, printerId: string): void;
  queuePending(queue: Int32, printerId: string): Int32;
  queueExpiredCount(queue: Int32): Int32;
  queueOverflowCount(queue: Int32): Int32;
  queueDrainedCount(queue: Int32): Int32;
  queueTick(queue: Int32): void;

  // --- cash drawer -------------------------------------------------------------------------

  printerDrawerCapabilities(printer: Int32): UnsafeObject;
  /** Blocks: reads the switch, pulses, watches, holds the cooldown. */
  drawerOpen(driver: Int32, printer: Int32, request: UnsafeObject): Promise<UnsafeObject>;
  /** Blocks. Never pulses, so it is safe where drawerOpen is refused. */
  drawerReadSensor(driver: Int32, printer: Int32, timeoutMs: Int32): Promise<UnsafeObject>;
  drawerCalibratePolarity(driver: Int32, printer: Int32, highMeansOpen: boolean): boolean;
  drawerPolarityCalibrated(driver: Int32, printer: Int32): boolean;
  drawerHighMeansOpen(driver: Int32, printer: Int32): boolean;

  // --- self-test, auto-detection, discovery ------------------------------------------------

  /** Blocks: prints ONE diagnostic ticket through the ordinary fenced engine. */
  selfTest(driver: Int32, printer: Int32, options: UnsafeObject): Promise<UnsafeObject | null>;
  /**
   * Blocks. Nothing prints and nothing fires. `requestId` correlates the 'detected' sink
   * messages that stream while it runs.
   */
  autoDetect(driver: Int32, options: UnsafeObject, requestId: Int32): Promise<Int32>;
  /** Blocks. The only bytes written are DLE EOT 1. */
  discover(driver: Int32, options: UnsafeObject, requestId: Int32): Promise<Int32>;
  detectedAt(driver: Int32, index: Int32): UnsafeObject | null;
  discoveredAt(driver: Int32, index: Int32): UnsafeObject | null;

  // --- custom method registration -----------------------------------------------------------

  registerCompletionMethod(driver: Int32, method: UnsafeObject): boolean;
  registerProbeStep(driver: Int32, step: UnsafeObject): boolean;
  registerBlockHandler(driver: Int32, handler: UnsafeObject): boolean;
  registerFormatter(driver: Int32, formatter: UnsafeObject): boolean;
  registerDrawerKick(driver: Int32, registration: UnsafeObject): boolean;

  // --- the receipt DSL (M19) ------------------------------------------------------------
  //
  // Both renders BLOCK, and not only because rendering costs time: a document may reach a
  // registered formatter or block handler, and those are the questions that travel out
  // through the sink and park a core thread until JavaScript answers. Running either of
  // these inline on the JS thread would be the exact deadlock PrinterDriverModule.h
  // describes — the JS thread waiting inside the ABI for an answer only the JS thread can
  // give. Hence a Promise on both, as with every other blocking entry point here.

  /**
   * Renders and returns `{ bytes, codePage, hasCut, cut, topFeedDots, bottomFeedDots,
   * report }`. Nothing prints and no job exists. Resolves null when there are no bytes;
   * the report is still readable with `renderReportAt`.
   */
  renderDocument(
    driver: Int32,
    printer: Int32,
    documentJson: string,
    modelJson: string,
    options: UnsafeObject
  ): Promise<UnsafeObject | null>;
  /**
   * The same render, submitted through the ordinary fenced engine. Resolves
   * `{ job, report }` — an ordinary job token — or null when nothing was submitted.
   */
  printDocumentJson(
    driver: Int32,
    printer: Int32,
    documentJson: string,
    modelJson: string,
    options: UnsafeObject
  ): Promise<UnsafeObject | null>;
  /** The last render's report by index, for a caller that would rather pull. */
  renderReportAt(driver: Int32, index: Int32): UnsafeObject | null;
  renderReportCount(driver: Int32): Int32;
  reportKindName(value: Int32): string;
  renderPathName(value: Int32): string;
}

/**
 * Resolve the installed TurboModule.
 *
 * Deliberately a function rather than the more common
 * `export default TurboModuleRegistry.getEnforcing<Spec>('PrinterDriver')` at module
 * scope: `getEnforcing` THROWS when the native module is absent, so a top-level call makes
 * `import 'printerdriver-react-native'` throw in every environment that is not a built RN
 * app — including this package's own unit tests. React Native's codegen locates a module
 * by the `TurboModuleRegistry.getEnforcing<Spec>('PrinterDriver')` call and the exported
 * `Spec` interface, both of which are still right here.
 */
export function getNativePrinterDriver(): Spec {
  return TurboModuleRegistry.getEnforcing<Spec>('PrinterDriver');
}

export default getNativePrinterDriver;
