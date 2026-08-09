/**
 * The service. Owns connections, queues and the persistent job store.
 */

import { EventStream } from './events.ts';
import { MatchKind } from './enums.ts';
import { ConfidenceGrade, CompletionAuthority } from './enums.ts';
import {
  marshalAutoDetectOptions,
  marshalDiscoverOptions,
  marshalDriverConfig,
  marshalTcpConfig,
  unmarshalDetectedPrinter,
  unmarshalDiscoveredDevice,
} from './marshal.ts';
import type { NativeDetectedPrinter, NativeDiscoveredDevice } from './marshal.ts';
import {
  addLogListener,
  nativeModule,
  openStream,
  setRequestHandler,
} from './native.ts';
import { PrintJob } from './PrintJob.ts';
import { Printer, PrinterDriverError } from './Printer.ts';
import { PrintQueue } from './PrintQueue.ts';
import {
  assertNonPrinting,
  pinLevelToNative,
} from './registrations.ts';
import type {
  BlockHandlerRegistration,
  CompletionMethodRegistration,
  DrawerKickRegistration,
  FormatterRegistration,
  ProbeStepRegistration,
} from './registrations.ts';
import type { CustomTransport } from './transports.ts';
import type {
  AutoDetectOptions,
  DetectedPrinter,
  DiscoverOptions,
  DiscoveredDevice,
  DriverConfig,
  QueuePolicy,
  TcpPrinterConfig,
} from './types.ts';

let nextTransportId = 1;

export class PrinterDriver {
  /** @internal */ readonly handle: number;

  private disposed = false;
  private readonly cleanups: (() => void)[] = [];

  private constructor(handle: number) {
    this.handle = handle;
  }

  /**
   * Creates the driver. Pass a `storageDirectory` in any app that must survive a restart:
   * without one there is no journal, so `findJob` after a relaunch finds nothing and a
   * job that was in flight when the app died can never be resurfaced as `unknown`.
   */
  static create(config?: DriverConfig): PrinterDriver {
    const module = nativeModule();
    const handle = module.create(marshalDriverConfig(config));
    if (handle === 0) {
      throw new PrinterDriverError(
        'pd_create failed. Only a storage directory that cannot be created can cause it.'
      );
    }
    const driver = new PrinterDriver(handle);
    if (config?.onLog !== undefined) {
      driver.cleanups.push(addLogListener(config.onLog));
    }
    return driver;
  }

  /** Why the last call on this driver returned nothing. "" when it succeeded. */
  get lastError(): string {
    return nativeModule().lastError(this.handle);
  }

  /**
   * The profile ids `addPrinter` accepts. Enumerated from the core rather than hardcoded,
   * so a profile added to the device database shows up without a wrapper release.
   */
  profileIds(): string[] {
    return nativeModule().profileIds();
  }

  /**
   * The two characters every verification token this driver issues starts with: which
   * driver instance owns an echo. Persisted in the storage directory, so it survives a
   * restart.
   */
  get instanceNonce(): string {
    return nativeModule().instanceNonce(this.handle);
  }

  /** The /24 around this host's primary address, or "" when it cannot be determined. */
  get localSubnet(): string {
    return nativeModule().localSubnet(this.handle);
  }

  // --- printers ------------------------------------------------------------------------

  addPrinter(config: TcpPrinterConfig): Printer {
    const module = nativeModule();
    const handle = module.addPrinterTcp(this.handle, marshalTcpConfig(config));
    if (handle === 0) throw new PrinterDriverError(module.lastError(this.handle));
    return new Printer(this.handle, handle);
  }

  /** Ad-hoc convenience, keyed by endpoint. */
  printer(host: string, widthDots = 576, profileId?: string): Printer {
    return this.addPrinter({ host, widthDots, profileId });
  }

  /**
   * A printer reached over a link JavaScript owns — a BLE library, an MFi session, a USB
   * bridge, a test double. See src/transports.ts for the thread contract.
   */
  addCustomPrinter(
    transport: CustomTransport,
    options?: { profileId?: string; widthDots?: number }
  ): Printer {
    const module = nativeModule();
    const transportId = nextTransportId;
    nextTransportId += 1;

    setRequestHandler('transport.connect', transportId, async () => ({
      ok: (await transport.connect()) === true,
    }));
    setRequestHandler('transport.write', transportId, async (payload) => ({
      written: await transport.write(payload.data as ArrayBuffer),
    }));
    setRequestHandler('transport.close', transportId, async () => {
      await transport.close();
      return {};
    });

    const handle = module.addPrinterCustom(
      this.handle,
      transportId,
      transport.description ?? '',
      options?.profileId ?? '',
      options?.widthDots ?? 0
    );
    if (handle === 0) {
      setRequestHandler('transport.connect', transportId, null);
      setRequestHandler('transport.write', transportId, null);
      setRequestHandler('transport.close', transportId, null);
      throw new PrinterDriverError(module.lastError(this.handle));
    }
    this.cleanups.push(() => {
      setRequestHandler('transport.connect', transportId, null);
      setRequestHandler('transport.write', transportId, null);
      setRequestHandler('transport.close', transportId, null);
    });
    return new Printer(this.handle, handle);
  }

  // --- jobs ----------------------------------------------------------------------------

  /** Any job this driver knows about, including ones reloaded from the journal. */
  findJob(key: string): PrintJob | null {
    const handle = nativeModule().findJob(this.handle, key);
    return handle === 0 ? null : new PrintJob(this.handle, handle);
  }

  /**
   * Paper -> job (docs/api.md §14). Resolves either of a job's four-character
   * verification identifiers, most-recent-first, including jobs reloaded from the
   * journal. This is what turns a `V:` code on a receipt an operator is holding into the
   * job that produced it.
   */
  jobByToken(token: string): PrintJob | null {
    const handle = nativeModule().jobByToken(this.handle, token);
    return handle === 0 ? null : new PrintJob(this.handle, handle);
  }

  // --- print queue -----------------------------------------------------------------------

  /**
   * The print-queue addon (docs/sdk-spec.md §12). A queue is NOT a retry engine: a job
   * that ends `unknown` blocks its printer's lane until somebody who has looked at the
   * paper calls `unblock`.
   */
  createQueue(policy?: QueuePolicy): PrintQueue {
    return PrintQueue.create(this.handle, policy);
  }

  // --- discovery and auto-detection --------------------------------------------------------

  /**
   * Discovery + identification + the printless subset of the capability probe, with the
   * stored-findings cache respected. NOTHING PRINTS AND NOTHING FIRES.
   *
   * Candidates arrive on the returned stream as they are finished; the promise resolves
   * with all of them once the sweep is done.
   */
  autoDetect(options?: AutoDetectOptions): {
    readonly candidates: EventStream<DetectedPrinter>;
    readonly done: Promise<DetectedPrinter[]>;
  } {
    const candidates = new EventStream<DetectedPrinter>();
    const found: DetectedPrinter[] = [];
    const stream = openStream((payload) => {
      const printer = unmarshalDetectedPrinter(
        payload.printer as unknown as NativeDetectedPrinter
      );
      found.push(printer);
      candidates.emit(printer);
    });
    const done = nativeModule()
      .autoDetect(this.handle, marshalAutoDetectOptions(options), stream.requestId)
      .then((count: number) => {
        stream.close();
        candidates.close();
        if (count < 0) throw new PrinterDriverError(this.lastError);
        return found;
      })
      .catch((error: unknown) => {
        stream.close();
        candidates.close();
        throw error;
      });
    return { candidates, done };
  }

  /**
   * The raw sweep underneath `autoDetect`: is something ESC/POS-shaped listening, and does
   * its backchannel reach me? The whole write side is DLE EOT 1 — every byte of it is
   * below 0x20, so none of it can print, on any device, ever.
   */
  discover(options?: DiscoverOptions): {
    readonly devices: EventStream<DiscoveredDevice>;
    readonly done: Promise<DiscoveredDevice[]>;
  } {
    const devices = new EventStream<DiscoveredDevice>();
    const found: DiscoveredDevice[] = [];
    const stream = openStream((payload) => {
      const device = unmarshalDiscoveredDevice(
        payload.device as unknown as NativeDiscoveredDevice
      );
      found.push(device);
      devices.emit(device);
    });
    const done = nativeModule()
      .discover(this.handle, marshalDiscoverOptions(options), stream.requestId)
      .then((count: number) => {
        stream.close();
        devices.close();
        if (count < 0) throw new PrinterDriverError(this.lastError);
        return found;
      })
      .catch((error: unknown) => {
        stream.close();
        devices.close();
        throw error;
      });
    return { devices, done };
  }

  /**
   * The last sweep's results by index, for a host that would rather pull than subscribe.
   * Every string is owned by the driver and refreshed by the next sweep.
   */
  detectedAt(index: number): DetectedPrinter | null {
    const raw = nativeModule().detectedAt(this.handle, index);
    return raw === null || raw === undefined
      ? null
      : unmarshalDetectedPrinter(raw as unknown as NativeDetectedPrinter);
  }

  discoveredAt(index: number): DiscoveredDevice | null {
    const raw = nativeModule().discoveredAt(this.handle, index);
    return raw === null || raw === undefined
      ? null
      : unmarshalDiscoveredDevice(raw as unknown as NativeDiscoveredDevice);
  }

  // --- custom method registration --------------------------------------------------------------

  /** docs/api.md §16, the marquee: a vendor idle/ack scheme as a real graded fence. */
  registerCompletionMethod(method: CompletionMethodRegistration): void {
    setRequestHandler('completion.fenceBytes', method.id, async (payload) => ({
      bytes: await method.fenceBytes(String(payload.jobToken)),
    }));
    setRequestHandler('completion.match', method.id, async (payload) => {
      const verdict = await method.match(payload.data as ArrayBuffer);
      return {
        kind: MatchKind[verdict.kind],
        token: verdict.kind === 'matched' ? verdict.token : '',
      };
    });
    const ok = nativeModule().registerCompletionMethod(this.handle, {
      id: method.id,
      grade: ConfidenceGrade[method.grade],
      authority: CompletionAuthority[method.authority],
      methodName: method.methodName ?? '',
    });
    this.assertRegistered(ok, `completion method ${method.id}`, () => {
      setRequestHandler('completion.fenceBytes', method.id, null);
      setRequestHandler('completion.match', method.id, null);
    });
  }

  registerProbeStep(step: ProbeStepRegistration): void {
    assertNonPrinting(step.requestBytes, `probe step ${step.id}`);
    setRequestHandler('probe.classify', step.id, async (payload) => {
      const finding = await step.classify(payload.response as ArrayBuffer);
      return { answered: finding.answered, label: finding.label };
    });
    const ok = nativeModule().registerProbeStep(this.handle, {
      id: step.id,
      requestBytes: step.requestBytes,
    });
    this.assertRegistered(ok, `probe step ${step.id}`, () => {
      setRequestHandler('probe.classify', step.id, null);
    });
  }

  registerBlockHandler(handler: BlockHandlerRegistration): void {
    setRequestHandler('block.render', handler.kind, async (payload) => {
      const rendered = await handler.render(
        String(payload.blockJson),
        String(payload.profileJson)
      );
      return rendered.ok
        ? { ok: true, bytes: rendered.bytes }
        : { ok: false, detail: rendered.detail };
    });
    const ok = nativeModule().registerBlockHandler(this.handle, { kind: handler.kind });
    this.assertRegistered(ok, `block handler ${handler.kind}`, () => {
      setRequestHandler('block.render', handler.kind, null);
    });
  }

  registerFormatter(formatter: FormatterRegistration): void {
    setRequestHandler('formatter.format', formatter.name, async (payload) => {
      const text = await formatter.format(
        String(payload.value),
        String(payload.args),
        String(payload.locale)
      );
      return text === null ? { handled: false, text: '' } : { handled: true, text };
    });
    const ok = nativeModule().registerFormatter(this.handle, { name: formatter.name });
    this.assertRegistered(ok, `formatter ${formatter.name}`, () => {
      setRequestHandler('formatter.format', formatter.name, null);
    });
  }

  registerDrawerKick(registration: DrawerKickRegistration): void {
    setRequestHandler('drawer.kickBytes', registration.id, async (payload) => ({
      bytes: await registration.kickBytes(Number(payload.channel), Number(payload.pulseMs)),
    }));
    const hasStatus =
      registration.statusRequest !== undefined && registration.statusParse !== undefined;
    if (hasStatus) {
      const statusRequest = registration.statusRequest as () => ArrayBuffer | Promise<ArrayBuffer>;
      const statusParse = registration.statusParse as NonNullable<
        DrawerKickRegistration['statusParse']
      >;
      setRequestHandler('drawer.statusRequest', registration.id, async () => ({
        bytes: await statusRequest(),
      }));
      setRequestHandler('drawer.statusParse', registration.id, async (payload) => ({
        pinHigh: pinLevelToNative(await statusParse(payload.response as ArrayBuffer)),
      }));
    }
    const ok = nativeModule().registerDrawerKick(this.handle, {
      id: registration.id,
      hasStatus,
    });
    this.assertRegistered(ok, `drawer kick ${registration.id}`, () => {
      setRequestHandler('drawer.kickBytes', registration.id, null);
      setRequestHandler('drawer.statusRequest', registration.id, null);
      setRequestHandler('drawer.statusParse', registration.id, null);
    });
  }

  // --- lifetime -----------------------------------------------------------------------------------

  /**
   * Stops every printer worker, waits for in-flight jobs to reach a terminal state, and
   * frees every printer and job handle this driver ever returned. Every `PrintJob` and
   * `Printer` object obtained from it is dead afterwards.
   */
  async destroy(): Promise<void> {
    if (this.disposed) return;
    this.disposed = true;
    for (const cleanup of this.cleanups.splice(0)) cleanup();
    await nativeModule().destroy(this.handle);
  }

  private assertRegistered(ok: boolean, what: string, undo: () => void): void {
    if (ok) return;
    undo();
    throw new PrinterDriverError(`${what} was refused: ${this.lastError}`);
  }
}
