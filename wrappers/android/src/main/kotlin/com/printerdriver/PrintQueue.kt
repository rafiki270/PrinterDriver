package com.printerdriver

import com.printerdriver.internal.NativeBridge

/**
 * M13b. The print-queue addon (docs/sdk-spec.md section 12), through `pd_queue_*`.
 *
 * Layered on the public API, never part of it. The core already contains the only queue
 * correctness requires -- one active job per printer, with a completion fence between
 * jobs -- and that part is not optional and not policy. Everything here is policy: holding
 * while a printer is unusable, draining on recovery, expiry, priority, depth limits.
 *
 * Three rules from section 12 are load-bearing, and none of them is implemented in Kotlin.
 * They live in the C++ addon behind the ABI, so this behaves identically to the Swift,
 * Dart and .NET surfaces:
 *
 *  1. **A queue is not a retry engine.** A job that ends [JobOutcome.UNKNOWN] blocks its
 *     printer's lane, and nothing further drains onto that printer until [unblock] is
 *     called by somebody who has looked at the paper. No timer clears it, because no timer
 *     can see a receipt.
 *  2. **Idempotency keys flow through**, all the way into the driver's own index, so a
 *     direct [Printer.print] of a key that is still parked returns the parked job.
 *  3. **No bypass.** Draining runs the identical engine path a direct print takes: same
 *     worker, same preflight, same fences, same confidence grading.
 *
 * Close this before the [PrinterDriver] it was built on.
 */

/** In what order a lane's waiting jobs are chosen. Mirrors `pd_drain_order`. */
enum class DrainOrder(internal val raw: Int) {
    /** Submission order. The safe default for tickets that must stay in sequence. */
    FIFO(0),

    /** Higher [QueueOptions.priority] first, submission order within equal priorities. */
    PRIORITY(1),
}

/** What the queue does with a job it cannot send yet. Mirrors `pd_queue_policy`. */
data class QueuePolicy(
    /** Park jobs while the printer is known to be offline, coverless or out of paper,
     *  instead of failing them one at a time. False makes the queue a pure serializer. */
    val holdWhileOffline: Boolean = true,

    /** Shelf life for a held job, in milliseconds; 0 means it never expires. A kitchen
     *  ticket must not print into a recovered kitchen half an hour late. */
    val defaultTtlMs: Int = 0,

    /** Held jobs per printer. 0 is unlimited, which recreates the printer's own buffer
     *  problem one layer up. */
    val maxDepth: Int = 64,

    val drainOrder: DrainOrder = DrainOrder.FIFO,
)

/** Per-job queue settings. The printing half mirrors [JobOptions]. */
data class QueueOptions(
    /** The idempotency key. A key that already has a job -- held, printing, or finished
     *  months ago -- returns that job and prints nothing. */
    val key: String? = null,

    /** 0 uses [QueuePolicy.defaultTtlMs]. */
    val ttlMs: Int = 0,

    /** Orders the waiting set only. A job already in flight is never preempted. */
    val priority: Int = 0,

    val cut: CutSetting = CutSetting.PROFILE,
    val openDrawer: Boolean = false,
    val preflight: PreflightMode = PreflightMode.STRICT,

    /** 0 uses the profile's completion timeout. */
    val timeoutMs: Int = 0,
)

/** A policy queue in front of one [PrinterDriver]. */
class PrintQueue internal constructor(
    private val driver: PrinterDriver,
    private var handle: Long,
) : AutoCloseable {

    /**
     * Enqueues raw bytes.
     *
     * The returned [PrintJob] is an ordinary job handle -- same id, same event stream --
     * already sent when the printer is usable and its lane is free, otherwise held, or
     * already terminal with [FailureReason.QUEUE_OVERFLOW] when the lane is full.
     *
     * Raw only for now; see NativeBridge.queueEnqueueRaw for why, and for what a caller
     * with a document does instead. Declared rather than silently missing.
     */
    fun enqueueRaw(
        printer: Printer,
        bytes: ByteArray,
        options: QueueOptions = QueueOptions(),
    ): PrintJob {
        val job = NativeBridge.queueEnqueueRaw(
            alive(),
            printer.handle,
            bytes,
            options.key,
            options.ttlMs,
            options.priority,
            options.cut.raw,
            options.openDrawer,
            options.preflight.raw,
            options.timeoutMs,
        )
        if (job == 0L) {
            throw PrinterDriverException("pd_queue_enqueue failed: ${driver.lastError}")
        }
        return PrintJob(driver, job)
    }

    /** Operator hold, independent of what the device is reporting. */
    fun pause(printerId: String) = NativeBridge.queuePause(alive(), printerId)

    fun resume(printerId: String) = NativeBridge.queueResume(alive(), printerId)

    fun isPaused(printerId: String): Boolean = NativeBridge.queueIsPaused(alive(), printerId)

    /** True once a job on this printer ended [JobOutcome.UNKNOWN]. Rule 1: nothing more
     *  drains onto that lane until somebody has looked at the paper and called [unblock]. */
    fun isBlocked(printerId: String): Boolean = NativeBridge.queueIsBlocked(alive(), printerId)

    fun unblock(printerId: String) = NativeBridge.queueUnblock(alive(), printerId)

    /** Held jobs. A null [printerId] counts every lane. */
    fun pending(printerId: String? = null): Long = NativeBridge.queuePending(alive(), printerId)

    val expiredCount: Long get() = NativeBridge.queueExpiredCount(alive())

    val overflowCount: Long get() = NativeBridge.queueOverflowCount(alive())

    val drainedCount: Long get() = NativeBridge.queueDrainedCount(alive())

    /** One expiry-and-drain pass on the calling thread. */
    fun tick() = NativeBridge.queueTick(alive())

    /** Stops the queue thread and frees the handle. Held jobs stay held and stay
     *  non-terminal: the queue does not invent an outcome for a job whose fate it does not
     *  know. Must run before the driver is closed. */
    override fun close() {
        val current = handle
        handle = 0L
        if (current != 0L) {
            NativeBridge.queueDestroy(current)
        }
    }

    private fun alive(): Long {
        check(handle != 0L) { "this PrintQueue has been closed" }
        return handle
    }
}
