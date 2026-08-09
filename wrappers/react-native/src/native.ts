/**
 * The single point where JavaScript meets the TurboModule, and the router for everything
 * the native side sends back.
 *
 * WHY THE MODULE IS RESOLVED LAZILY
 *   `TurboModuleRegistry.getEnforcing` throws when the native module is not installed, and
 *   importing a package must not throw. More importantly, it can only be called inside a
 *   React Native runtime — so if this file imported it eagerly, none of the logic above it
 *   could be unit-tested anywhere else. src/index.ts installs the real loader; a test calls
 *   `setNativeModule()` with a double. Nothing else in src/ imports 'react-native'.
 *
 * THREADING (the contract cpp/PrinterDriverModule.h implements)
 *   Core callbacks arrive on printer worker threads and transport reader threads. The
 *   native module hops every one of them onto the JS thread through the TurboModule
 *   `CallInvoker` before it reaches the sink below, so every listener, transport callback
 *   and registration callback in this package runs on the JS thread and may safely touch
 *   React state.
 *
 *   Messages carrying a `requestId` are QUESTIONS: a core thread is blocked waiting.
 *   Answer exactly once with `respond`. This module does that for you — a handler simply
 *   returns a value (or a promise of one) and the plumbing here forwards it. A handler that
 *   throws or never settles is answered with the registration's documented failure value
 *   after the native deadline, which is why a printer never hangs on a buggy JS callback.
 */

import type { Spec } from './NativePrinterDriver.ts';

export type NativeSpec = Spec;

type Loader = () => Spec;

let loader: Loader | null = null;
let override: Spec | null = null;
let cached: Spec | null = null;
let sinkInstalled = false;

/** Installed by src/index.ts. Separated so tests never import 'react-native'. */
export function setNativeModuleLoader(next: Loader | null): void {
  loader = next;
  cached = null;
}

/** Test seam: install a double, or `null` to go back to the real module. */
export function setNativeModule(module: Spec | null): void {
  override = module;
  cached = null;
  sinkInstalled = false;
}

export class NativeModuleUnavailableError extends Error {
  constructor() {
    super(
      "printerdriver-react-native's native module is not installed. It contains native " +
        'code, so: (1) run `pod install` after adding the package on iOS and rebuild; ' +
        '(2) rebuild the Android app so CMake compiles the core; (3) on Expo, use a ' +
        'development build (`npx expo prebuild` / EAS) — Expo Go cannot load native ' +
        'modules. The New Architecture must be enabled (React Native 0.71+).'
    );
    this.name = 'NativeModuleUnavailableError';
  }
}

export function nativeModule(): Spec {
  if (override !== null) return override;
  if (cached !== null) return cached;
  if (loader === null) throw new NativeModuleUnavailableError();
  try {
    cached = loader();
  } catch {
    throw new NativeModuleUnavailableError();
  }
  ensureSink();
  return cached;
}

// --- the sink: everything native -> JS ------------------------------------------------------

export interface RawJobEventMessage {
  job: number;
  state: number;
  confidence: number;
  hasReason: number;
  reason: number;
  monotonicMs: number;
}

export interface RawDeviceEventMessage {
  printer: number;
  event: number;
}

type SinkPayload = Record<string, unknown>;

type JobListener = (message: RawJobEventMessage) => void;
type DeviceListener = (message: RawDeviceEventMessage) => void;
type StreamListener = (payload: SinkPayload) => void;
type RequestHandler = (payload: SinkPayload) => unknown | Promise<unknown>;

const jobListeners = new Map<number, Set<JobListener>>();
const deviceListeners = new Map<number, Set<DeviceListener>>();
const streamListeners = new Map<number, StreamListener>();
const requestHandlers = new Map<string, RequestHandler>();
const logListeners = new Set<(message: string) => void>();

/** Request kinds and the value shape `respond` expects for each. */
export const requestKinds = [
  'transport.connect',
  'transport.write',
  'transport.close',
  'completion.fenceBytes',
  'completion.match',
  'probe.classify',
  'block.render',
  'formatter.format',
  'drawer.kickBytes',
  'drawer.statusRequest',
  'drawer.statusParse',
] as const;
export type RequestKind = (typeof requestKinds)[number];

/**
 * The value the native side uses when a handler is missing, throws, or rejects. Each one
 * is the registration's own documented failure, never an invented success:
 * a transport that cannot connect reports failure, a matcher reports `notMine`, a
 * formatter declines, a block handler degrades.
 */
const fallbackForKind: Readonly<Record<RequestKind, unknown>> = {
  'transport.connect': { ok: false },
  'transport.write': { written: -1 },
  'transport.close': {},
  'completion.fenceBytes': { bytes: new ArrayBuffer(0) },
  'completion.match': { kind: 1, token: '' },
  'probe.classify': { answered: false, label: '' },
  'block.render': { ok: false, detail: 'the JavaScript block handler failed' },
  'formatter.format': { handled: false, text: '' },
  'drawer.kickBytes': { bytes: new ArrayBuffer(0) },
  'drawer.statusRequest': { bytes: new ArrayBuffer(0) },
  'drawer.statusParse': { pinHigh: -1 },
};

function isRequestKind(kind: string): kind is RequestKind {
  return (requestKinds as readonly string[]).includes(kind);
}

/**
 * Routes one sink message. Exported so the routing can be tested directly with a fake
 * module — it is the part of the bridge most worth a test and the hardest to reach from
 * an app.
 */
export function dispatchSinkMessage(kind: string, payload: SinkPayload): void {
  if (kind === 'job') {
    const message = payload as unknown as RawJobEventMessage;
    const listeners = jobListeners.get(message.job);
    if (listeners !== undefined) for (const listener of [...listeners]) listener(message);
    return;
  }
  if (kind === 'device') {
    const message = payload as unknown as RawDeviceEventMessage;
    const listeners = deviceListeners.get(message.printer);
    if (listeners !== undefined) for (const listener of [...listeners]) listener(message);
    return;
  }
  if (kind === 'log') {
    const message = String(payload.message ?? '');
    for (const listener of [...logListeners]) listener(message);
    return;
  }
  if (kind === 'detected' || kind === 'discovered') {
    const listener = streamListeners.get(Number(payload.requestId));
    if (listener !== undefined) listener(payload);
    return;
  }
  if (isRequestKind(kind)) {
    answerRequest(kind, payload);
    return;
  }
  // An unknown kind means the native library is newer than this package. Dropping it is
  // right for a notification and wrong for a question, and a question always carries a
  // requestId — so answer that one with nothing rather than blocking a printer forever.
  if (typeof payload.requestId === 'number') {
    nativeModule().respond(payload.requestId, {});
  }
}

function answerRequest(kind: RequestKind, payload: SinkPayload): void {
  const requestId = Number(payload.requestId);
  const handlerKey = `${kind}:${String(payload.transport ?? payload.id ?? payload.kind ?? payload.name ?? '')}`;
  const handler = requestHandlers.get(handlerKey);
  const settle = (value: unknown): void => {
    try {
      nativeModule().respond(requestId, value as object);
    } catch {
      // The driver is gone. Nothing is waiting any more.
    }
  };
  if (handler === undefined) {
    settle(fallbackForKind[kind]);
    return;
  }
  let produced: unknown;
  try {
    produced = handler(payload);
  } catch {
    settle(fallbackForKind[kind]);
    return;
  }
  if (produced instanceof Promise) {
    produced.then(settle, () => settle(fallbackForKind[kind]));
    return;
  }
  settle(produced);
}

function ensureSink(): void {
  if (sinkInstalled) return;
  const module = override ?? cached;
  if (module === null || module === undefined) return;
  module.setEventSink((kind: string, payload: object) => {
    dispatchSinkMessage(kind, payload as SinkPayload);
  });
  sinkInstalled = true;
}

/** Installs the sink against a double. Tests call this instead of `nativeModule()`. */
export function installSinkForTesting(): void {
  sinkInstalled = false;
  ensureSink();
}

// --- registration tables -------------------------------------------------------------------

export function addJobListener(job: number, listener: JobListener): () => void {
  let set = jobListeners.get(job);
  if (set === undefined) {
    set = new Set();
    jobListeners.set(job, set);
  }
  set.add(listener);
  return () => {
    const current = jobListeners.get(job);
    if (current === undefined) return;
    current.delete(listener);
    if (current.size === 0) jobListeners.delete(job);
  };
}

export function addDeviceListener(printer: number, listener: DeviceListener): () => void {
  let set = deviceListeners.get(printer);
  if (set === undefined) {
    set = new Set();
    deviceListeners.set(printer, set);
  }
  set.add(listener);
  return () => {
    const current = deviceListeners.get(printer);
    if (current === undefined) return;
    current.delete(listener);
    if (current.size === 0) deviceListeners.delete(printer);
  };
}

export function addLogListener(listener: (message: string) => void): () => void {
  logListeners.add(listener);
  return () => {
    logListeners.delete(listener);
  };
}

let nextStreamId = 1;

export function openStream(listener: StreamListener): { requestId: number; close: () => void } {
  const requestId = nextStreamId;
  nextStreamId += 1;
  streamListeners.set(requestId, listener);
  return {
    requestId,
    close: () => {
      streamListeners.delete(requestId);
    },
  };
}

export function setRequestHandler(
  kind: RequestKind,
  id: string | number,
  handler: RequestHandler | null
): void {
  const key = `${kind}:${String(id)}`;
  if (handler === null) requestHandlers.delete(key);
  else requestHandlers.set(key, handler);
}

/** Test-only: forget every listener and handler between cases. */
export function resetBridgeForTesting(): void {
  jobListeners.clear();
  deviceListeners.clear();
  streamListeners.clear();
  requestHandlers.clear();
  logListeners.clear();
  nextStreamId = 1;
}
