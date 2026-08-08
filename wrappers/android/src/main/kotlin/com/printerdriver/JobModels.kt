package com.printerdriver

import com.printerdriver.internal.PdLog

/**
 * One step of a job's history (pd_job_event). [reason] is only non-null on failure
 * transitions (pd.h: `has_reason` is 0 for every non-failure transition). [monotonicMs]
 * comes from a steady clock -- pd.h: "the wall clock may step and job timing must
 * not" -- so do not treat it as a wall-clock timestamp.
 */
data class JobEvent(
    val state: JobState,
    val confidence: ConfidenceLevel,
    val reason: FailureReason?,
    val monotonicMs: Long
) {
    internal companion object {
        fun fromRaw(state: Int, confidence: Int, hasReason: Boolean, reason: Int, monotonicMs: Long): JobEvent = JobEvent(
            state = JobState.fromRaw(state),
            confidence = ConfidenceLevel.fromRaw(confidence),
            reason = if (hasReason) FailureReason.fromRaw(reason) else null,
            monotonicMs = monotonicMs
        )
    }
}

/**
 * The terminal answer (pd_job_result) -- the tri-state from docs/api.md §1.4 and §4.
 * There is deliberately no boolean anywhere here: collapsing [Unknown] into either
 * [Done] or [Failed] is exactly the bug that produces duplicate kitchen tickets, so
 * apps are made to handle three cases by the compiler (exhaustive `when`), not by
 * convention.
 *
 * [confidence] is carried on all three outcomes, not only [Done]: pd.h's own struct
 * comment is explicit that on [Failed] and [Unknown] it "records how far up the
 * evidence ladder the job got before it stopped, which is what an operator needs to
 * decide about a reprint." docs/api.md §4's prose sketch only shows confidence on
 * `.done(confidence)`; this wrapper follows the actual pd_job_result struct (which has
 * one `confidence` field regardless of `outcome`) rather than the simplified sketch,
 * since that is the documented, intentional shape of the real ABI it binds -- not an
 * oversight to flatten away.
 *
 * Note on scope: a `grade`/`authority`/`method` triple appears in
 * docs/device-database.md's "Confidence grades for every route" as a description of
 * the confidence *model* generally (its A-E grades are the same idea [ConfidenceLevel]
 * already encodes as one closed enum), not as literal fields of pd_job_result -- the
 * actual struct in pd.h, and pd::JobResult in core/include/printerdriver/types.hpp,
 * both carry exactly (outcome, confidence, reason). This wrapper mirrors that struct;
 * it does not invent grade/authority/method fields the C ABI does not provide.
 */
sealed class JobResult {
    /** Reached DoneSoftware (or PhysicallyVerified later). [confidence] says what that
     *  claim rests on -- e.g. CUT_FAULT_FREE on a GS(H) printer vs TRANSPORT_ACCEPTED
     *  on a write-only one. Never inflated by the SDK. */
    data class Done(val confidence: ConfidenceLevel) : JobResult()

    /** FailedKnown: nothing printed, or the failure was confirmed (preflight refusal,
     *  transport unreachable, cutter fault...). Safe to resubmit the same key. */
    data class Failed(val reason: FailureReason, val confidence: ConfidenceLevel) : JobResult()

    /** Bytes were sent, no acknowledgement (timeout, crash, link drop). NOT success,
     *  NOT failure -- surface to an operator; resolve via [Printer.forceReprint] or
     *  manual confirmation. */
    data class Unknown(val confidence: ConfidenceLevel) : JobResult()

    internal companion object {
        // Raw pd_job_outcome values (pd.h): DONE=0, FAILED=1, UNKNOWN=2.
        fun fromRaw(outcome: Int, confidence: Int, reason: Int): JobResult {
            val level = ConfidenceLevel.fromRaw(confidence)
            return when (outcome) {
                0 -> Done(level)
                1 -> Failed(FailureReason.fromRaw(reason), level)
                2 -> Unknown(level)
                else -> {
                    // An outcome value this wrapper has never heard of is, by
                    // construction, not known to be Done or Failed -- Unknown is the
                    // only outcome that is safe to report without inventing a claim
                    // the native layer never made. Mirrors JobState/ConfidenceLevel's
                    // UNRECOGNIZED handling, just at the sealed-class boundary instead
                    // of an enum's.
                    PdLog.w("Unrecognized pd_job_outcome raw value: $outcome; reporting Unknown")
                    Unknown(level)
                }
            }
        }
    }
}

/**
 * Job submission options (pd_job_options). All-zeroes/all-defaults is a valid, safe
 * configuration everywhere in this ABI (pd.h: "no default trades durability for
 * speed") -- these Kotlin defaults mirror the C struct's zero-value defaults exactly.
 */
data class JobOptions(
    /** Idempotency key (order/ticket UUID). `null` -> the SDK generates one: feedback
     *  still works, but there is no dedupe protection across restarts. Fleet/POS apps
     *  should always pass one (docs/api.md §3). */
    val key: String? = null,
    val cut: Cut = Cut.PROFILE,
    val openDrawer: Boolean = false,
    val preflight: Preflight = Preflight.STRICT,
    /** Per-phase completion-wait budget in ms. 0 -> the printer's profile default. */
    val timeoutMs: Int = 0
)
