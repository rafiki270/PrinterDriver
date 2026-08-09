/**
 * A handle to one configured device: transport + width + capability profile.
 */

import { EventStream } from './events.ts';
import type {
  CommandLanguage,
  CompletionMechanism,
  DeviceEvent,
  Provenance,
} from './enums.ts';
import {
  commandLanguageFromNative,
  completionMechanismFromNative,
  deviceEventFromNative,
  provenanceFromNative,
} from './enums.ts';
import {
  describeReportEntry,
  marshalDrawerRequest,
  marshalJobOptions,
  marshalJsonDocument,
  marshalPayload,
  marshalRenderOptions,
  marshalReprintOptions,
  marshalSelfTestOptions,
  unmarshalDeviceStatus,
  unmarshalDrawerCapabilities,
  unmarshalDrawerReading,
  unmarshalDrawerResult,
  unmarshalDetectionSummary,
  unmarshalJobResult,
  unmarshalRenderedDocument,
  unmarshalReport,
} from './marshal.ts';
import type {
  NativeDeviceStatus,
  NativeDetectionSummary,
  NativeDrawerCapabilities,
  NativeDrawerReading,
  NativeDrawerResult,
  NativeJobResult,
  NativePrintDocumentResult,
  NativeRenderResult,
} from './marshal.ts';
import { addDeviceListener, nativeModule } from './native.ts';
import { toArrayBuffer } from './payload.ts';
import { PrintJob } from './PrintJob.ts';
import type {
  DetectionSummary,
  DeviceStatus,
  DrawerCapabilities,
  DrawerReading,
  DrawerRequest,
  DrawerResult,
  JobEvent,
  JobOptions,
  JobResult,
  JsonDocument,
  Payload,
  RenderOptions,
  RenderedDocument,
  ReportEntry,
  ReprintOptions,
  SelfTestOptions,
} from './types.ts';

export class PrinterDriverError extends Error {
  constructor(message: string) {
    super(message.length === 0 ? 'the native call failed and reported no reason' : message);
    this.name = 'PrinterDriverError';
  }
}

/**
 * The three hard refusals of docs/receipt-dsl.md carry the report as well as the reason:
 * JSON that is not JSON, a structure that is not a document, and a template with no model
 * each produce at least one entry explaining itself.
 */
function renderFailure(error: string, report: readonly ReportEntry[]): PrinterDriverError {
  const lines = report.map(describeReportEntry);
  return new PrinterDriverError([error, ...lines].filter((line) => line.length > 0).join('\n'));
}

export interface SelfTestResult {
  /** The ordinary tri-state job result. A `done` at grade A IS the end-to-end proof. */
  readonly result: JobResult;
  readonly detection: DetectionSummary;
  readonly key: string;
  /** The four GS ( H characters printed as `V:` and inside the QR. */
  readonly printToken: string;
  /** The ticket exactly as it was laid out, lines separated by '\n'. */
  readonly ticketText: string;
  readonly job: PrintJob;
}

export class Printer {
  /** @internal */ readonly driverHandle: number;
  /** @internal */ readonly handle: number;

  private deviceStream: EventStream<DeviceEvent> | null = null;
  private deviceSubscription: (() => void) | null = null;

  /** @internal */
  constructor(driverHandle: number, handle: number) {
    this.driverHandle = driverHandle;
    this.handle = handle;
  }

  /** Stable across restarts; derived from the endpoint when the caller did not set one. */
  get id(): string {
    return nativeModule().printerId(this.handle);
  }

  get widthDots(): number {
    return nativeModule().printerWidthDots(this.handle);
  }

  /** Which ordered fence this printer's profile answers. */
  get completion(): CompletionMechanism {
    return completionMechanismFromNative(nativeModule().printerCompletion(this.handle));
  }

  /**
   * What that fence is actually worth: `documented` (the manufacturer's own command
   * documentation lists it), `probed` (this driver asked the hardware and it answered),
   * or `unverified` (neither — which is what "ESC/POS compatible" on a datasheet amounts
   * to). Not a confidence level, and it changes nothing a job reports; it answers
   * "should I trust this printer's fence before I have printed anything?".
   */
  get completionProvenance(): Provenance {
    return provenanceFromNative(nativeModule().printerCompletionProvenance(this.handle));
  }

  /** Anything but `escPos` is refused with `unsupported`, before a byte is written. */
  get language(): CommandLanguage {
    return commandLanguageFromNative(nativeModule().printerLanguage(this.handle));
  }

  // --- printing --------------------------------------------------------------------------

  /**
   * The one submit call. Re-submitting a key that already has a job does not print: it
   * returns that job's handle, whatever state it is in.
   */
  print(payload: Payload, options?: JobOptions): PrintJob {
    const module = nativeModule();
    const handle = module.print(
      this.driverHandle,
      this.handle,
      marshalPayload(payload),
      marshalJobOptions(options)
    );
    if (handle === 0) throw new PrinterDriverError(module.lastError(this.driverHandle));
    return new PrintJob(this.driverHandle, handle);
  }

  /**
   * The closure form of docs/api.md §12. `onProgress` receives every JobEvent in order;
   * the returned promise settles once, with the terminal tri-state result. Sugar over the
   * same stream — there is no second code path.
   */
  async send(
    payload: Payload,
    options?: JobOptions & { onProgress?: (event: JobEvent) => void }
  ): Promise<JobResult> {
    const job = this.print(payload, options);
    if (options?.onProgress !== undefined) {
      const onProgress = options.onProgress;
      job.events.on((event) => onProgress(event));
    }
    return job.result;
  }

  /**
   * A deliberate duplicate of an already-submitted key: reuses the original payload and
   * prepends the REPRINT banner and attempt counter. Throws when the key is unknown, or
   * when its job was reconstructed from the journal — those records carry what happened
   * to a job, never what it contained.
   */
  forceReprint(key: string, options?: ReprintOptions): PrintJob {
    const module = nativeModule();
    // pd_force_reprint is pd_force_reprint_opts with an all-zeroes pd_reprint_options,
    // i.e. with the banner on. Call the plain one when the caller did not ask about the
    // banner, so the simple path goes through the simple ABI entry point.
    const handle =
      options?.banner === undefined
        ? module.forceReprint(this.driverHandle, this.handle, key, marshalJobOptions(options))
        : module.forceReprintOpts(
            this.driverHandle,
            this.handle,
            key,
            marshalReprintOptions(options)
          );
    if (handle === 0) throw new PrinterDriverError(module.lastError(this.driverHandle));
    return new PrintJob(this.driverHandle, handle);
  }

  // --- the receipt DSL (docs/receipt-dsl.md) -----------------------------------------------
  //
  // No rendering logic lives here. Which blocks a profile can draw, what a missing model
  // path does, how `meta.cut` reaches a job: all of it is in the core behind
  // `pd_render_document` and `pd_print_document_json`. Both are async because the render may
  // call back into JavaScript-registered formatters and block handlers — see
  // src/NativePrinterDriver.ts.

  /**
   * Renders a receipt-DSL document against this printer's capability profile.
   *
   * **Nothing prints and no job exists.** This is the preview path: the bytes a printer
   * would receive, plus every degradation the profile forced, before any paper is
   * committed.
   *
   * `document` and `model` each take the JSON text an app already has or the plain object
   * it would stringify anyway. `model` is the parameter model bound into a template
   * (`{{path.to.value}}`, `each`, `if`); omit it for a document that carries its own
   * content.
   *
   * Rejects with `PrinterDriverError` when the JSON is not JSON, the structure is not a
   * document, or a template arrived with no model — a receipt full of `{{order.total}}` is
   * worse than no receipt, because it looks like one. Everything softer is a `ReportEntry`
   * in `report` and still renders.
   */
  async renderDocument(
    document: JsonDocument,
    model?: JsonDocument | null,
    options?: RenderOptions
  ): Promise<RenderedDocument> {
    const raw = await nativeModule().renderDocument(
      this.driverHandle,
      this.handle,
      marshalJsonDocument(document, 'document'),
      marshalJsonDocument(model, 'model'),
      marshalRenderOptions(options)
    );
    if (raw === null || raw === undefined) {
      throw new PrinterDriverError(nativeModule().lastError(this.driverHandle));
    }
    const value = raw as unknown as NativeRenderResult;
    // The report is read on the failure path too: a caller that wants to show what went
    // wrong gets the same list it would show for a degradation.
    if (value.ok !== 1) throw renderFailure(value.error, unmarshalReport(value.report));
    return unmarshalRenderedDocument(value);
  }

  /**
   * Renders a receipt-DSL document and prints it.
   *
   * The job is an ordinary `PrintJob`: same worker, same preflight, same completion fence,
   * same confidence grading, same idempotency-key dedupe as `print`. There is no second
   * engine — a template job proves exactly what a raster job proves.
   *
   * The document's `meta` reaches the job through `options`: omitting them lets the
   * document decide its own cut and margins, and any field set explicitly wins over it.
   * What the renderer declared is on the returned job, as `job.renderReport`.
   *
   * Rejects with `PrinterDriverError` when there are no bytes to send, and submits nothing
   * in that case: a malformed document never becomes a blank receipt.
   */
  async printDocument(
    document: JsonDocument,
    model?: JsonDocument | null,
    options?: JobOptions
  ): Promise<PrintJob> {
    const raw = await nativeModule().printDocumentJson(
      this.driverHandle,
      this.handle,
      marshalJsonDocument(document, 'document'),
      marshalJsonDocument(model, 'model'),
      marshalJobOptions(options)
    );
    if (raw === null || raw === undefined) {
      throw new PrinterDriverError(nativeModule().lastError(this.driverHandle));
    }
    const value = raw as unknown as NativePrintDocumentResult;
    const report = unmarshalReport(value.report);
    if (value.job === 0) throw renderFailure(value.error, report);
    const job = new PrintJob(this.driverHandle, value.job);
    job.setRenderReport(report);
    return job;
  }

  // --- status ------------------------------------------------------------------------------

  /** Last known state. Never a live query, so it cannot block behind a print. */
  status(): DeviceStatus {
    const raw = nativeModule().printerStatus(this.driverHandle, this.handle);
    return unmarshalDeviceStatus(raw as unknown as NativeDeviceStatus);
  }

  /** Queues a status round trip behind any active job and waits for the answer. */
  async refreshStatus(timeoutMs = 0): Promise<DeviceStatus> {
    const raw = await nativeModule().printerRefreshStatus(
      this.driverHandle,
      this.handle,
      timeoutMs
    );
    return unmarshalDeviceStatus(raw as unknown as NativeDeviceStatus);
  }

  /** Resolves once this printer's queue is empty and its active job is terminal. */
  drain(): Promise<void> {
    return nativeModule().printerDrain(this.driverHandle, this.handle);
  }

  /**
   * The per-printer device stream that replaces availability ping-polling. Subscribing is
   * idempotent.
   */
  get events(): EventStream<DeviceEvent> {
    if (this.deviceStream === null) {
      this.deviceStream = new EventStream<DeviceEvent>();
      this.deviceSubscription = addDeviceListener(this.handle, (message) => {
        this.deviceStream?.emit(deviceEventFromNative(message.event));
      });
      nativeModule().subscribeDevice(this.driverHandle, this.handle);
    }
    return this.deviceStream;
  }

  /** Stops delivering device events. The printer itself is unaffected. */
  releaseEvents(): void {
    this.deviceSubscription?.();
    this.deviceSubscription = null;
    this.deviceStream?.close();
    this.deviceStream = null;
  }

  // --- custom transports ---------------------------------------------------------------------

  /**
   * Deliver bytes the JS-owned link received. Safe to call at any time, including while a
   * write is in flight — that is the normal case, a status answer arriving while the next
   * chunk goes out. Returns false when nothing was listening (the core has not connected
   * yet, or has already closed), which is information rather than an error.
   */
  feedBytes(bytes: ArrayBuffer | ArrayBufferView): boolean {
    return nativeModule().transportFeedBytes(this.handle, toArrayBuffer(bytes));
  }

  /**
   * Report that the link dropped for a reason other than an explicit close — out of
   * range, the OS tore the channel down, pairing revoked. Surfaces as `connectionLost`
   * and fails any job waiting on a fence instead of leaving it to time out.
   */
  reportLinkDropped(message = ''): boolean {
    return nativeModule().transportLinkDropped(this.handle, message);
  }

  // --- cash drawer -------------------------------------------------------------------------------

  /** The profile's drawer facet. A caller that reads nothing else should read `kickable`. */
  drawerCapabilities(): DrawerCapabilities {
    const raw = nativeModule().printerDrawerCapabilities(this.handle);
    return unmarshalDrawerCapabilities(raw as unknown as NativeDrawerCapabilities);
  }

  /**
   * The unverified kick: one ESC p, no sensor read, no verdict. This is `pd_open_cash_drawer`
   * — the legacy one-liner. Prefer `openDrawer()`, which reads the switch first and comes
   * back with a state instead of a shrug.
   */
  openCashDrawer(): void {
    nativeModule().openCashDrawer(this.driverHandle, this.handle);
  }

  /**
   * The verified opening sequence: read the switch (an already-open drawer is never
   * pulsed again), pulse on the printer's own worker, watch the switch, hold the
   * manufacturer cooldown. Never a boolean — `kickSentUnverified` is a real answer.
   */
  async openDrawer(request?: DrawerRequest): Promise<DrawerResult> {
    const raw = await nativeModule().drawerOpen(
      this.driverHandle,
      this.handle,
      marshalDrawerRequest(request)
    );
    return unmarshalDrawerResult(raw as unknown as NativeDrawerResult);
  }

  /** One non-destructive read. Never pulses, so it is safe where `openDrawer` is refused. */
  async readDrawerSensor(timeoutMs = 0): Promise<DrawerReading> {
    const raw = await nativeModule().drawerReadSensor(
      this.driverHandle,
      this.handle,
      timeoutMs
    );
    return unmarshalDrawerReading(raw as unknown as NativeDrawerReading);
  }

  /**
   * Records which pin level means "open" for the drawer physically attached to this
   * printer, and persists it. The calling sequence is the operator one: ask for the drawer
   * to be closed, read the sensor, ask for it to be opened, read again, and pass the level
   * observed while it was open. Returns false when it applies to this process only.
   */
  calibrateDrawerPolarity(highMeansOpen: boolean): boolean {
    return nativeModule().drawerCalibratePolarity(
      this.driverHandle,
      this.handle,
      highMeansOpen
    );
  }

  get drawerPolarityCalibrated(): boolean {
    return nativeModule().drawerPolarityCalibrated(this.driverHandle, this.handle);
  }

  /** Meaningful only while `drawerPolarityCalibrated` is true. */
  get drawerHighMeansOpen(): boolean {
    return nativeModule().drawerHighMeansOpen(this.driverHandle, this.handle);
  }

  // --- self-test ------------------------------------------------------------------------------------

  /**
   * Prints ONE diagnostic ticket through the full fenced engine — the paper is the
   * detection report — and resolves when the job is terminal. A `done` at grade A here is
   * the statement that the whole stack works end to end on this unit, over this interface
   * path.
   */
  async selfTest(options?: SelfTestOptions): Promise<SelfTestResult> {
    const module = nativeModule();
    const raw = await module.selfTest(
      this.driverHandle,
      this.handle,
      marshalSelfTestOptions(options)
    );
    if (raw === null || raw === undefined) {
      throw new PrinterDriverError(module.lastError(this.driverHandle));
    }
    const value = raw as unknown as {
      result: NativeJobResult;
      detection: NativeDetectionSummary;
      key: string;
      printToken: string;
      ticketText: string;
      job: number;
    };
    return {
      result: unmarshalJobResult(value.result),
      detection: unmarshalDetectionSummary(value.detection),
      key: value.key,
      printToken: value.printToken,
      ticketText: value.ticketText,
      job: new PrintJob(this.driverHandle, value.job),
    };
  }
}
