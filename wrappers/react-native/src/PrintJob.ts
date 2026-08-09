/**
 * One submitted job: an event stream, a queryable state, and one terminal tri-state
 * result. Durable across restarts — `driver.findJob(key)` returns the same job after the
 * app has been killed and relaunched.
 */

import { EventStream } from './events.ts';
import type { ConfidenceLevel, JobState } from './enums.ts';
import { confidenceLevelFromNative, jobStateFromNative } from './enums.ts';
import { unmarshalJobEvent, unmarshalJobResult } from './marshal.ts';
import type { NativeJobResult } from './marshal.ts';
import { addJobListener, nativeModule } from './native.ts';
import type { JobEvent, JobResult, ReportEntry } from './types.ts';

export class PrintJob {
  /** @internal */ readonly driverHandle: number;
  /** @internal */ readonly handle: number;

  private readonly stream = new EventStream<JobEvent>();
  private subscription: (() => void) | null = null;
  private pendingResult: Promise<JobResult> | null = null;
  private report: readonly ReportEntry[] = [];

  /** @internal */
  constructor(driverHandle: number, handle: number) {
    this.driverHandle = driverHandle;
    this.handle = handle;
  }

  /** The job's own UUID. Stable for the life of the driver. */
  get id(): string {
    return nativeModule().jobId(this.handle);
  }

  /** The idempotency key. Generated when the caller did not supply one. */
  get key(): string {
    return nativeModule().jobKey(this.handle);
  }

  /**
   * The verification identifiers of docs/api.md §14: the token printed as `V:` and the
   * one this job's cut fence carried. Both are "" on a printer whose completion mechanism
   * is not GS ( H — there is no wire token to promote — and until the job reaches a
   * worker.
   */
  get printToken(): string {
    return nativeModule().jobPrintToken(this.handle);
  }

  get cutToken(): string {
    return nativeModule().jobCutToken(this.handle);
  }

  /** 1 for a first send; incremented by a fresh attempt after a terminal failure. */
  get attempt(): number {
    return nativeModule().jobAttempt(this.handle);
  }

  get state(): JobState {
    return jobStateFromNative(nativeModule().jobCurrentState(this.handle));
  }

  get confidence(): ConfidenceLevel {
    return confidenceLevelFromNative(nativeModule().jobConfidence(this.handle));
  }

  get isTerminal(): boolean {
    return nativeModule().jobIsTerminal(this.handle);
  }

  /**
   * Every transition, in order, ending with a terminal one — always. Subscribing is
   * idempotent and replays whatever has already been recorded, so a listener attached
   * after `print()` returned still sees `queued`.
   */
  get events(): EventStream<JobEvent> {
    this.ensureSubscribed();
    return this.stream;
  }

  /**
   * The terminal result. Waits indefinitely by default; `timeoutMs` returns null instead
   * of the result when the budget passes, which is NOT the same as the job failing — the
   * job is still running and its own timeout still applies.
   *
   * Note the shape: a tri-state discriminated union with no success boolean. An
   * exhaustive `switch (result.outcome)` is the point (docs/api.md §1.4).
   */
  async awaitResult(timeoutMs = 0): Promise<JobResult | null> {
    this.ensureSubscribed();
    const raw = await nativeModule().jobAwait(this.driverHandle, this.handle, timeoutMs);
    if (raw === null || raw === undefined) return null;
    return unmarshalJobResult(raw as unknown as NativeJobResult);
  }

  /** The same wait, as a property, memoised. `await job.result`. */
  get result(): Promise<JobResult> {
    if (this.pendingResult === null) {
      this.pendingResult = this.awaitResult(0).then((result) => {
        if (result === null) {
          throw new Error('jobAwait with no timeout returned nothing; the driver was destroyed');
        }
        return result;
      });
    }
    return this.pendingResult;
  }

  /**
   * What the renderer declared on the way to this job, for a job submitted with
   * `printer.printDocument(...)`. Empty for every other job, and for a template job where
   * every block rendered exactly as written.
   *
   * Worth reading on the success path: a receipt that printed with a dropped barcode is a
   * receipt that printed, and this is the only place that says so.
   */
  get renderReport(): readonly ReportEntry[] {
    return this.report;
  }

  /** @internal Set by `printer.printDocument`, which is the only caller that has one. */
  setRenderReport(entries: readonly ReportEntry[]): void {
    this.report = entries;
  }

  /** Stops delivering events for this job. The job itself is unaffected. */
  release(): void {
    if (this.subscription !== null) {
      this.subscription();
      this.subscription = null;
    }
    this.stream.close();
  }

  private ensureSubscribed(): void {
    if (this.subscription !== null) return;
    this.subscription = addJobListener(this.handle, (message) => {
      const event = unmarshalJobEvent(message);
      this.stream.emit(event);
      if (isTerminalState(event.state)) this.stream.close();
    });
    nativeModule().subscribeJob(this.driverHandle, this.handle);
  }
}

/**
 * The three terminal states. `heldOffline` is emphatically not one of them: a queued job
 * parked behind an offline printer has not finished, and the queue never invents an
 * outcome for a job whose fate it does not know.
 */
const terminalStates: ReadonlySet<JobState> = new Set<JobState>([
  'doneSoftware',
  'physicallyVerified',
  'failedKnown',
  'unknown',
]);

export function isTerminalState(state: JobState): boolean {
  return terminalStates.has(state);
}
