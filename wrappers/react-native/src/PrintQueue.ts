/**
 * The print-queue addon through the ABI (docs/sdk-spec.md §12).
 *
 * The whole addon is bound here and not a convenience subset, because the three rules
 * below ARE the addon, and a wrapper that could only enqueue would quietly reintroduce
 * each one as a bug:
 *
 *   1. A QUEUE IS NOT A RETRY ENGINE. A job that ends `unknown` blocks its printer's lane,
 *      and nothing further drains onto that printer until `unblock` is called by somebody
 *      who has looked at the paper. There is no timer that clears it, because no timer can
 *      see a receipt.
 *   2. IDEMPOTENCY KEYS FLOW THROUGH. Enqueuing a key that already has a job — held,
 *      printing, or finished months ago — returns that job and prints nothing.
 *   3. NO BYPASS. Draining runs the identical engine path a direct `print` takes: same
 *      worker, same preflight, same fences, same confidence grading. A queued job is an
 *      ordinary `PrintJob` throughout, with `heldOffline` appearing in its event stream
 *      while the bytes are parked.
 *
 * Lifetime: a queue MUST be destroyed before the driver that owns it. It is the one handle
 * in this ABI the caller owns, because it is an addon rather than part of the driver's
 * object graph.
 */

import { marshalPayload, marshalQueueOptions, marshalQueuePolicy } from './marshal.ts';
import { nativeModule } from './native.ts';
import { PrintJob } from './PrintJob.ts';
import type { Printer } from './Printer.ts';
import { PrinterDriverError } from './Printer.ts';
import type { Payload, QueueOptions, QueuePolicy } from './types.ts';

export class PrintQueue {
  /** @internal */ readonly driverHandle: number;
  /** @internal */ readonly handle: number;

  private disposed = false;

  private constructor(driverHandle: number, handle: number) {
    this.driverHandle = driverHandle;
    this.handle = handle;
  }

  /** @internal Use `driver.createQueue(policy)`. */
  static create(driverHandle: number, policy?: QueuePolicy): PrintQueue {
    const module = nativeModule();
    const handle = module.queueCreate(driverHandle, marshalQueuePolicy(policy));
    if (handle === 0) throw new PrinterDriverError(module.lastError(driverHandle));
    return new PrintQueue(driverHandle, handle);
  }

  /**
   * Returns an ordinary `PrintJob`. It is already sent when the printer is usable and its
   * lane is free; otherwise it is in `heldOffline`, or already terminal with
   * `queueOverflow` when the lane is full.
   */
  enqueue(printer: Printer, payload: Payload, options?: QueueOptions): PrintJob {
    this.assertLive();
    const module = nativeModule();
    const handle = module.queueEnqueue(
      this.handle,
      printer.handle,
      marshalPayload(payload),
      marshalQueueOptions(options)
    );
    if (handle === 0) throw new PrinterDriverError(module.lastError(this.driverHandle));
    return new PrintJob(this.driverHandle, handle);
  }

  /** Operator hold, independent of what the device is reporting. */
  pause(printerId: string): void {
    this.assertLive();
    nativeModule().queuePause(this.handle, printerId);
  }

  resume(printerId: string): void {
    this.assertLive();
    nativeModule().queueResume(this.handle, printerId);
  }

  isPaused(printerId: string): boolean {
    this.assertLive();
    return nativeModule().queueIsPaused(this.handle, printerId);
  }

  /**
   * True once a job on this printer ended `unknown`. The lane drains nothing more until
   * `unblock` — rule 1, and the reason this is a function and not a timeout.
   */
  isBlocked(printerId: string): boolean {
    this.assertLive();
    return nativeModule().queueIsBlocked(this.handle, printerId);
  }

  /** Call this only after somebody has looked at the paper. */
  unblock(printerId: string): void {
    this.assertLive();
    nativeModule().queueUnblock(this.handle, printerId);
  }

  /** Held jobs. An omitted `printerId` counts every lane. */
  pending(printerId = ''): number {
    this.assertLive();
    return nativeModule().queuePending(this.handle, printerId);
  }

  get expiredCount(): number {
    this.assertLive();
    return nativeModule().queueExpiredCount(this.handle);
  }

  get overflowCount(): number {
    this.assertLive();
    return nativeModule().queueOverflowCount(this.handle);
  }

  get drainedCount(): number {
    this.assertLive();
    return nativeModule().queueDrainedCount(this.handle);
  }

  /**
   * Runs one expiry-and-drain pass. The queue's own thread does this on every device event
   * and whenever a TTL comes due; this is for hosts that would rather pump it themselves,
   * and for tests.
   */
  tick(): void {
    this.assertLive();
    nativeModule().queueTick(this.handle);
  }

  /**
   * Stops the queue thread. Held jobs stay held and stay NON-TERMINAL: the queue does not
   * invent an outcome for a job whose fate it does not know. Must be called before
   * `driver.destroy()`.
   */
  async destroy(): Promise<void> {
    if (this.disposed) return;
    this.disposed = true;
    await nativeModule().queueDestroy(this.handle);
  }

  private assertLive(): void {
    if (this.disposed) throw new PrinterDriverError('this PrintQueue has been destroyed');
  }
}
