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
 * pd_job_result also carries the evidence triple of docs/device-database.md
 * "Confidence grades for every route": [ConfidenceGrade], [CompletionAuthority] and the
 * command that produced the claim. The ABI carries them on all three outcomes -- a
 * refusal's grade E / TransportOnly is as much information as a Done's grade A -- so
 * every case surfaces them; never a bare success.
 */
sealed class JobResult {
    /** Reached DoneSoftware (or PhysicallyVerified later). [confidence] says what that
     *  claim rests on -- e.g. CUT_FAULT_FREE on a GS(H) printer vs TRANSPORT_ACCEPTED
     *  on a write-only one. Never inflated by the SDK.
     *
     *  [grade] says what class of evidence it is, [authority] who made the claim, and
     *  [method] which command produced it ("GS(H) fn48") -- the string a support
     *  engineer needs six months later; "none" when nothing was confirmed. */
    data class Done(
        val confidence: ConfidenceLevel,
        val grade: ConfidenceGrade,
        val authority: CompletionAuthority,
        val method: String
    ) : JobResult()

    /** FailedKnown: nothing printed, or the failure was confirmed (preflight refusal,
     *  transport unreachable, cutter fault...). Safe to resubmit the same key. */
    data class Failed(
        val reason: FailureReason,
        val confidence: ConfidenceLevel,
        val grade: ConfidenceGrade,
        val authority: CompletionAuthority,
        val method: String
    ) : JobResult()

    /** Bytes were sent, no acknowledgement (timeout, crash, link drop). NOT success,
     *  NOT failure -- surface to an operator; resolve via [Printer.forceReprint] or
     *  manual confirmation. */
    data class Unknown(
        val confidence: ConfidenceLevel,
        val grade: ConfidenceGrade,
        val authority: CompletionAuthority,
        val method: String
    ) : JobResult()

    internal companion object {
        // Raw pd_job_outcome values (pd.h): DONE=0, FAILED=1, UNKNOWN=2.
        fun fromRaw(
            outcome: Int,
            confidence: Int,
            reason: Int,
            grade: Int,
            authority: Int,
            method: String
        ): JobResult {
            val level = ConfidenceLevel.fromRaw(confidence)
            return when (outcome) {
                0 -> Done(level, ConfidenceGrade.fromRaw(grade), CompletionAuthority.fromRaw(authority), method)
                1 -> Failed(FailureReason.fromRaw(reason), level,
                    ConfidenceGrade.fromRaw(grade), CompletionAuthority.fromRaw(authority), method)
                2 -> Unknown(level,
                    ConfidenceGrade.fromRaw(grade), CompletionAuthority.fromRaw(authority), method)
                else -> {
                    // An outcome value this wrapper has never heard of is, by
                    // construction, not known to be Done or Failed -- Unknown is the
                    // only outcome that is safe to report without inventing a claim
                    // the native layer never made. Mirrors JobState/ConfidenceLevel's
                    // UNRECOGNIZED handling, just at the sealed-class boundary instead
                    // of an enum's.
                    PdLog.w("Unrecognized pd_job_outcome raw value: $outcome; reporting Unknown")
                    Unknown(level, ConfidenceGrade.fromRaw(grade),
                        CompletionAuthority.fromRaw(authority), method)
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
    val timeoutMs: Int = 0,
    /** Blank paper fed before the first content line -- tear-off clearance,
     *  presentation space (docs/receipt-dsl.md "Margins"). */
    val topFeedDots: Int = 0,
    /** The *total* whitespace between the last content and the cut. The core feeds
     *  max(the profile's blade clearance, this), so this can only ever add paper: a
     *  value below the hardware minimum is ignored rather than allowed to slice through
     *  a trailing QR. */
    val bottomFeedDots: Int = 0,
    /** Print the receipt verification identifier in the ticket trailer -- the ORDER
     *  line and the trailer QR (docs/api.md §14). On by default. Turning it off
     *  suppresses the ink, not the evidence: the token is still journaled and
     *  [PrinterDriver.jobByToken] still resolves it. */
    val printVerificationId: Boolean = true
)

/**
 * Options for a deliberate duplicate (pd_reprint_options).
 *
 * [banner] prints `*** REPRINT / POSSIBLE DUPLICATE ***` and the attempt counter, and is
 * on by default. Turning it off is a per-call, deliberate act for a receipt where the
 * banner is inappropriate -- a customer-facing copy. A kitchen ticket should never turn
 * it off: the banner is what lets staff bin the duplicate instead of cooking it twice.
 */
data class ReprintOptions(
    val job: JobOptions = JobOptions(),
    val banner: Boolean = true
)
