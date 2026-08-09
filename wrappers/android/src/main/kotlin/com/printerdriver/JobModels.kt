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

// --- M14: cash drawer (docs/cash-drawer.md) -------------------------------------------

/**
 * What a printer's drawer port is, and what is known about it (pd_drawer_capabilities).
 *
 * A separate peripheral facet, not part of [DeviceStatus] and not part of the printer's
 * own capabilities: the connector pinout, the drive voltage and the command the firmware
 * implements are three independent facts, and none follows from anything the printer does
 * with paper.
 */
data class DrawerCapabilities(
    /** This model has a drawer port at all. */
    val present: Boolean,
    /** The electrical classification. Nothing is ever fired on
     *  [DrawerPortStandard.UNKNOWN]. */
    val portStandard: DrawerPortStandard,
    /** Volts, or 0 where the manufacturer does not document it -- not the same as "low". */
    val voltage: Int,
    val maxCurrentMa: Int,
    val channelCount: Int,
    /** 3 on the Epson arrangement, 6 on Star's; 0 when unestablished. */
    val sensorPin: Int,
    val kickMethod: DrawerKickMethod,
    val defaultPulseMs: Int,
    val maxPulseMs: Int,
    /** Held between two pulses so a retry loop cannot keep a solenoid energised. */
    val cooldownMs: Int,
    /** False on models whose drawer output cannot fire while the mechanism prints; the
     *  pulse is then ordered strictly behind everything already queued. */
    val canKickDuringPrint: Boolean,
    val statusAvailable: Boolean,
    val statusMethod: DrawerStatusMethod,
    /** Two drive outputs and one switch input: both channels kick independently while
     *  the only readable fact is that *some* attached drawer is open. */
    val sharedBetweenDrawers: Boolean,
    /** With the optional external buzzer enabled, the pulse that would fire the drawer
     *  sounds the buzzer instead. Never assume both coexist. */
    val sharedWithBuzzer: Boolean,
    /** Deliberately two columns: a documented 24 V port can sit beside an unproven pulse
     *  command, and the XP-S260M is exactly that case. */
    val electricalProvenance: Provenance,
    val commandsProvenance: Provenance,
    /** Whether this SDK may put a pulse on the wire: a method it can drive **and** an
     *  established electrical standard. A caller that reads nothing else reads this. */
    val kickable: Boolean
) {
    internal companion object {
        /** Builds one from the 18-element int array NativeBridge.drawerCapabilities
         *  returns, in the exact field order pd_drawer_capabilities declares them. */
        fun fromRaw(raw: IntArray): DrawerCapabilities {
            require(raw.size == 18) { "expected 18 pd_drawer_capabilities fields, got ${raw.size}" }
            return DrawerCapabilities(
                present = raw[0] != 0,
                portStandard = DrawerPortStandard.fromRaw(raw[1]),
                voltage = raw[2],
                maxCurrentMa = raw[3],
                channelCount = raw[4],
                sensorPin = raw[5],
                kickMethod = DrawerKickMethod.fromRaw(raw[6]),
                defaultPulseMs = raw[7],
                maxPulseMs = raw[8],
                cooldownMs = raw[9],
                canKickDuringPrint = raw[10] != 0,
                statusAvailable = raw[11] != 0,
                statusMethod = DrawerStatusMethod.fromRaw(raw[12]),
                sharedBetweenDrawers = raw[13] != 0,
                sharedWithBuzzer = raw[14] != 0,
                electricalProvenance = Provenance.fromRaw(raw[15]),
                commandsProvenance = Provenance.fromRaw(raw[16]),
                kickable = raw[17] != 0
            )
        }
    }
}

/** The outcome of one opening sequence (pd_drawer_result) -- a state, never a boolean. */
data class DrawerResult(
    val state: DrawerState,
    val previousState: DrawerState,
    val channel: Int,
    /** 0 when no pulse was emitted at all: already open, or refused. */
    val pulseMs: Int,
    /** Pulse to verdict -- the "sensor changed 143 ms after kick" number. */
    val elapsedMs: Int
) {
    /** The one thing worth branching on: the switch was seen changing. */
    val verified: Boolean get() = state == DrawerState.OPEN_VERIFIED

    internal companion object {
        fun fromRaw(raw: IntArray): DrawerResult {
            require(raw.size == 5) { "expected 5 pd_drawer_result fields, got ${raw.size}" }
            return DrawerResult(
                state = DrawerState.fromRaw(raw[0]),
                previousState = DrawerState.fromRaw(raw[1]),
                channel = raw[2],
                pulseMs = raw[3],
                elapsedMs = raw[4]
            )
        }
    }
}

/** One non-destructive read of the drawer switch (pd_drawer_reading). */
data class DrawerReading(
    /** The profile documents a readable switch on this port. */
    val available: Boolean,
    /** The device actually replied within the timeout. */
    val answered: Boolean,
    /** The raw sense level, or null when nothing answered. */
    val pinHigh: Boolean?,
    /** True until this printer's polarity has been measured. While it is true, [state]
     *  stays [DrawerState.UNKNOWN] however clear the level is: whether the line reads
     *  high or low when the drawer is open depends on the drawer that is plugged in. */
    val needsCalibration: Boolean,
    /** The interpretation, where there is one. */
    val state: DrawerState
) {
    internal companion object {
        fun fromRaw(raw: IntArray): DrawerReading {
            require(raw.size == 5) { "expected 5 pd_drawer_reading fields, got ${raw.size}" }
            return DrawerReading(
                available = raw[0] != 0,
                answered = raw[1] != 0,
                pinHigh = if (raw[2] < 0) null else raw[2] != 0,
                needsCalibration = raw[3] != 0,
                state = DrawerState.fromRaw(raw[4])
            )
        }
    }
}
