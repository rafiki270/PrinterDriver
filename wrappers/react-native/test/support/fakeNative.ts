/**
 * A recording stand-in for the TurboModule, so the whole JavaScript layer can be exercised
 * on a plain Node host.
 *
 * WHAT THIS IS NOT: evidence about the native module. It proves the JS side calls the
 * right ABI entry point with the right marshalled arguments and interprets the answers
 * correctly. Whether cpp/PrinterDriverModule.cpp actually implements those methods is a
 * separate question, answered (partly) by scripts/check_rn_cpp_syntax.sh and (fully) only
 * by a real device. README "Verification status" is explicit about the split.
 */

import type { Spec } from '../../src/NativePrinterDriver.ts';
import { nativeMethodNames } from '../../src/methodNames.ts';

export interface RecordedCall {
  readonly method: string;
  readonly args: readonly unknown[];
}

export interface FakeNative {
  readonly module: Spec;
  readonly calls: RecordedCall[];
  /** Fixed answers, by method name. A function is called with the arguments. */
  readonly answers: Map<string, unknown | ((...args: unknown[]) => unknown)>;
  /** The sink the code under test installed, if any. */
  sink: ((kind: string, payload: object) => void) | null;
  /** Everything answered through `respond`, in order. */
  readonly responses: { requestId: number; value: unknown }[];
  callsTo(method: string): RecordedCall[];
  lastCall(method: string): RecordedCall | undefined;
}

const defaultAnswers: Record<string, unknown> = {
  create: 1,
  addPrinterTcp: 10,
  addPrinterCustom: 11,
  print: 100,
  forceReprint: 101,
  forceReprintOpts: 102,
  queueCreate: 200,
  queueEnqueue: 201,
  findJob: 0,
  jobByToken: 0,
  lastError: '',
  jobId: 'job-uuid',
  jobKey: 'order-1',
  jobPrintToken: 'K73F',
  jobCutToken: 'K740',
  printerId: 'tcp:192.168.1.101:9100',
  instanceNonce: 'K7',
  localSubnet: '192.168.1.0/24',
  profileIds: ['generic', 'xprinter_s_series'],
  jobAttempt: 1,
  jobCurrentState: 0,
  jobConfidence: 0,
  jobIsTerminal: false,
  transportFeedBytes: true,
  transportLinkDropped: true,
  printerWidthDots: 576,
  printerCompletion: 0,
  printerCompletionProvenance: 1,
  printerLanguage: 0,
  queuePending: 0,
  queueExpiredCount: 0,
  queueOverflowCount: 0,
  queueDrainedCount: 0,
  queueIsPaused: false,
  queueIsBlocked: false,
  drawerCalibratePolarity: true,
  drawerPolarityCalibrated: false,
  drawerHighMeansOpen: false,
  registerCompletionMethod: true,
  registerProbeStep: true,
  registerBlockHandler: true,
  registerFormatter: true,
  registerDrawerKick: true,
  codePageAt: 0,
  autoDetect: 0,
  discover: 0,
  detectedAt: null,
  discoveredAt: null,
  selfTest: null,
  jobAwait: null,
};

export function createFakeNative(): FakeNative {
  const calls: RecordedCall[] = [];
  const answers = new Map<string, unknown | ((...args: unknown[]) => unknown)>(
    Object.entries(defaultAnswers)
  );
  const responses: { requestId: number; value: unknown }[] = [];

  const fake: Record<string, unknown> = {};

  for (const method of nativeMethodNames) {
    fake[method] = (...args: unknown[]): unknown => {
      calls.push({ method, args });
      const answer = answers.get(method);
      const value = typeof answer === 'function' ? answer(...args) : answer;
      // Every method whose spec return type is a Promise must hand back a promise; the
      // simplest faithful rule is that a recorded answer which is already a promise stays
      // one, and the async methods are listed here explicitly.
      return asyncMethods.has(method) ? Promise.resolve(value ?? null) : value;
    };
  }

  const result: FakeNative = {
    module: fake as unknown as Spec,
    calls,
    answers,
    sink: null,
    responses,
    callsTo: (method: string) => calls.filter((call) => call.method === method),
    lastCall: (method: string) => calls.filter((call) => call.method === method).at(-1),
  };

  fake.setEventSink = (sink: (kind: string, payload: object) => void): void => {
    result.sink = sink;
  };
  fake.respond = (requestId: number, value: unknown): void => {
    responses.push({ requestId, value });
  };

  return result;
}

export const asyncMethods = new Set<string>([
  'destroy',
  'printerRefreshStatus',
  'printerDrain',
  'jobAwait',
  'queueDestroy',
  'drawerOpen',
  'drawerReadSensor',
  'selfTest',
  'autoDetect',
  'discover',
]);
