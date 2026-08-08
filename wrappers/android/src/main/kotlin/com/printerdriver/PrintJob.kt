package com.printerdriver

import com.printerdriver.internal.NativeBridge
import com.printerdriver.internal.NativeJobEventCallback
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.transformWhile
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext

/**
 * One submitted job (docs/api.md §2): event stream + queryable state, durable across
 * restarts. Two [PrintJob] instances wrapping the same underlying pd_job* compare
 * equal ([equals]/[hashCode] are handle-based) -- deliberately, since pd.h's own
 * pointer-equality dedupe guarantee ("the same underlying job always maps to the same
 * pd_job pointer, so idempotency-key dedupe is visible as pointer equality") is
 * exactly the property a re-wrap (e.g. two [Printer.print] calls with the same [key],
 * or a [PrinterDriver.findJob] after a fresh submit) should preserve at this layer
 * too, without needing a Kotlin-side interning cache to do it.
 */
class PrintJob internal constructor(
    private val driver: PrinterDriver,
    internal val handle: Long
) {
    val id: String get() { driver.checkOpen(); return NativeBridge.jobId(handle) }
    val key: String get() { driver.checkOpen(); return NativeBridge.jobKey(handle) }

    /** The receipt verification identifier this attempt printed under -- the four
     *  characters the ticket carries as `V:` (docs/api.md §14). `null` until the job
     *  reaches a worker, and `null` for good on a printer whose fence is not GS(H):
     *  the identifier *is* the wire token. Resolve one with [PrinterDriver.jobByToken]. */
    val printToken: String? get() = NativeBridge.jobPrintToken(handle).ifEmpty { null }

    /** The identifier the job's cut fence carried. Same rules as [printToken]. */
    val cutToken: String? get() = NativeBridge.jobCutToken(handle).ifEmpty { null }

    /** 1 on first submission; incremented by each [Printer.forceReprint]. */
    val attempt: Int get() { driver.checkOpen(); return NativeBridge.jobAttempt(handle) }

    val state: JobState get() { driver.checkOpen(); return JobState.fromRaw(NativeBridge.jobCurrentState(handle)) }
    val confidence: ConfidenceLevel get() { driver.checkOpen(); return ConfidenceLevel.fromRaw(NativeBridge.jobConfidence(handle)) }
    val isTerminal: Boolean get() { driver.checkOpen(); return NativeBridge.jobIsTerminal(handle) }

    /**
     * The job's full event history, replayed, then live -- ends with a terminal event,
     * always (pd.h). Collecting this twice subscribes twice: pd_subscribe_job's replay
     * is per-call, so each collector independently gets the complete ordered history
     * from the start, exactly as if it had subscribed first. That is a deliberate
     * consequence of this being backed by [callbackFlow] (cold) rather than a shared/
     * hot flow -- see README.md "Threading contract" for why this wrapper does not
     * multicast a single native subscription across collectors.
     *
     * The native callback (see printerdriver_jni.cpp) only ever calls `trySend` on this
     * flow's channel; it never calls back into any native accessor. [isTerminal] is
     * checked below, in the [transformWhile] operator, specifically so that check runs
     * on the *collector's* coroutine -- never inside the native trampoline's call stack
     * -- because pd.h forbids a
     * callback from calling back into any pd_* function on the same driver ("the
     * worker thread it is running on is the thread that would have to service that
     * call"). This is not a cosmetic ordering choice; doing the isTerminal check inside
     * the callback itself would violate that contract.
     */
    val events: Flow<JobEvent> = callbackFlow {
        driver.checkOpen()
        val callback = NativeJobEventCallback { state, confidence, hasReason, reason, monotonicMs ->
            trySend(JobEvent.fromRaw(state, confidence, hasReason, reason, monotonicMs))
        }
        NativeBridge.subscribeJob(driver.handle, handle, callback)
        awaitClose {
            // No pd_unsubscribe_job exists (see NativeBridge.subscribeJob's doc). The
            // GlobalRef this subscription holds on the native side is released when
            // the owning PrinterDriver is closed, not when collection stops -- a
            // cancelled collector simply stops reading from this channel; the native
            // callback keeps firing (harmlessly, into a closed channel) until then.
        }
    }.transformWhile { event ->
        emit(event)
        !isTerminal
    }

    /**
     * Waits for the terminal result (pd_job_await). Implemented as a bounded poll
     * rather than a single indefinite native wait so that cancelling the calling
     * coroutine is noticed within [RESULT_POLL_INTERVAL_MS] instead of not at all --
     * pd_job_await is a blocking native call with no cancellation hook of its own, so
     * cooperative cancellation has to come from never blocking on it for longer than
     * one poll interval at a time.
     */
    suspend fun result(): JobResult = withContext(Dispatchers.IO) {
        while (isActive) {
            driver.checkOpen()
            val raw = NativeBridge.jobAwait(driver.handle, handle, RESULT_POLL_INTERVAL_MS)
            if (raw != null) {
                // The method string is fetched only once the result exists, so the
                // extra call is on the settled path and never on the polling one.
                return@withContext JobResult.fromRaw(
                    raw[0], raw[1], raw[2], raw[3], raw[4],
                    NativeBridge.jobMethod(driver.handle, handle)
                )
            }
        }
        throw CancellationException("PrintJob.result() was cancelled")
    }

    override fun equals(other: Any?): Boolean = other is PrintJob && other.handle == handle
    override fun hashCode(): Int = handle.hashCode()
    override fun toString(): String = "PrintJob(id=$id, key=$key, state=$state)"

    private companion object {
        const val RESULT_POLL_INTERVAL_MS = 250
    }
}
