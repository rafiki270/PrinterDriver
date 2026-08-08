package com.printerdriver

import com.printerdriver.internal.PdLog

// Closed enums mirrored verbatim from capi/include/printerdriver/pd.h, one-for-one,
// with explicit raw values -- never derived from Kotlin's own `ordinal` -- so that
// pd_code_page's non-contiguous values (the ESC t n operand) round-trip correctly and
// every other enum stays byte-for-byte pinned to its C constant regardless of member
// reordering here.
//
// docs/api.md §1.3: "wrappers cannot add, drop, or partially implement values." This
// wrapper cannot enforce that as strongly as pd_capi.cpp's static_asserts do (that
// build error is C++-compiler-enforced against the core; this is a separately
// compiled AAR against a separately compiled .so), so every `fromRaw` below adds one
// member the header does NOT define: UNRECOGNIZED. It exists only for the case where
// the native library loaded at runtime is a newer/older version than the Kotlin
// classes calling it (e.g. mismatched AAR/.so versions on the classpath) and reports a
// raw int this enum has no member for. Silently mapping that to the "nearest" known
// member -- or to whatever member happens to sit at that ordinal -- is exactly the
// kind of quiet misreporting the tri-state JobOutcome (see JobResult.kt) exists to
// prevent one layer up; UNRECOGNIZED extends the same discipline to every enum.

/** pd::JobState -- docs/sdk-spec.md §5, mirroring docs/techspec.md §5.1. */
enum class JobState(internal val raw: Int) {
    QUEUED(0),
    PREFLIGHT_OK(1),
    SEND_STARTED(2),
    BYTES_SENT(3),
    PRINT_CONFIRMED(4),
    CUT_COMMAND_PROCESSED(5),
    DONE_SOFTWARE(6),
    PHYSICALLY_VERIFIED(7),
    FAILED_KNOWN(8),
    UNKNOWN(9),

    /** Produced only by the print-queue addon (docs/sdk-spec.md §12); the core this
     *  wrapper binds never emits it. Included so the enum ships complete from day one. */
    HELD_OFFLINE(10),

    /** Not a real pd_job_state value -- see the file-level note above. */
    UNRECOGNIZED(-1);

    companion object {
        internal fun fromRaw(raw: Int): JobState = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_job_state raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/** pd::ConfidenceLevel -- what evidence backs the current claim. Never inflated by the
 *  SDK (docs/api.md §4); ordering is meaningful (higher = stronger evidence) but this
 *  wrapper does not add comparison sugar the core itself does not define. */
enum class ConfidenceLevel(internal val raw: Int) {
    TRANSPORT_ACCEPTED(0),
    PRINTER_HEALTHY(1),
    PRINT_CONFIRMED(2),
    CUT_PROCESSED(3),
    CUT_FAULT_FREE(4),
    PHYSICALLY_VERIFIED(5),
    UNRECOGNIZED(-1);

    companion object {
        internal fun fromRaw(raw: Int): ConfidenceLevel = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_confidence_level raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/** pd::DeviceEvent -- the per-printer stream that replaces availability polling. */
enum class DeviceEvent(internal val raw: Int) {
    ONLINE(0),
    OFFLINE(1),
    COVER_OPEN(2),
    COVER_CLOSED(3),
    PAPER_OUT(4),
    PAPER_NEAR_END(5),
    PAPER_OK(6),
    CUTTER_ERROR(7),
    RECOVERABLE_ERROR(8),
    UNRECOVERABLE_ERROR(9),
    CONNECTION_LOST(10),
    CONNECTION_RESTORED(11),

    /** A GS(H) echo arrived carrying a token this driver never issued: something else
     *  is writing to the same printer (docs/api.md §14, docs/sdk-spec.md §14). The echo
     *  is attributed to no job and satisfies no fence. One printer has exactly one
     *  connection owner; this is what a violation of that looks like from the inside. */
    FOREIGN_WRITER_DETECTED(12),
    UNRECOGNIZED(-1);

    companion object {
        internal fun fromRaw(raw: Int): DeviceEvent = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_device_event raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/** pd::FailureReason. Note [UNKNOWN] (raw 8) is a real, documented core value ("the
 *  reason itself could not be determined") -- distinct from [UNRECOGNIZED], which
 *  means this wrapper has never heard of the raw int at all. Do not conflate them. */
enum class FailureReason(internal val raw: Int) {
    NONE(0),
    TRANSPORT_UNREACHABLE(1),
    PREFLIGHT_COVER_OPEN(2),
    PREFLIGHT_PAPER_OUT(3),
    PREFLIGHT_HARDWARE_ERROR(4),
    TIMEOUT_AWAITING_COMPLETION(5),
    CUTTER_FAULT(6),
    UNSUPPORTED(7),
    UNKNOWN(8),

    /** Print-queue addon only (docs/sdk-spec.md §12). */
    EXPIRED(9),
    QUEUE_OVERFLOW(10),

    UNRECOGNIZED(-1);

    companion object {
        internal fun fromRaw(raw: Int): FailureReason = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_failure_reason raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/** pd::CutSetting -- [PROFILE] means "whatever this printer's cutter does" (the
 *  per-printer capability profile decides; this wrapper never does). Input-only today
 *  (set on [JobOptions]), but mirrored with the same fromRaw/UNRECOGNIZED shape as
 *  every other enum for consistency and forward-compatibility. */
enum class Cut(internal val raw: Int) {
    PROFILE(0),
    PARTIAL(1),
    FULL(2),
    NONE(3),
    UNRECOGNIZED(-1);

    companion object {
        internal fun fromRaw(raw: Int): Cut = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_cut raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/** pd::PreflightMode. */
enum class Preflight(internal val raw: Int) {
    /** Refuse on cover-open/paper-out, failing the job as [JobResult.Failed] rather
     *  than sending into a printer known to be broken. The default (docs/api.md §3). */
    STRICT(0),
    SKIP(1),
    UNRECOGNIZED(-1);

    companion object {
        internal fun fromRaw(raw: Int): Preflight = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_preflight raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/** pd::escpos::Alignment -- values are the `n` operand of ESC a n. Input-only, used by
 *  [DocumentOp.Align]. */
enum class Alignment(internal val raw: Int) {
    LEFT(0),
    CENTER(1),
    RIGHT(2),
    UNRECOGNIZED(-1);

    companion object {
        internal fun fromRaw(raw: Int): Alignment = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_alignment raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/**
 * pd::escpos::CodePage. Values are the `n` operand of ESC t n, so they are
 * deliberately non-contiguous (pd.h: "PD_CODE_PAGE_COUNT is the number of members
 * this SDK supports, not a past-the-end sentinel"). Because every member here already
 * carries its exact [raw] value, this wrapper does not need pd.h's own
 * pd_code_page_at() iteration helper -- Kotlin's [entries] already enumerates the
 * complete, correct set without a native round trip.
 */
enum class CodePage(internal val raw: Int) {
    PC437(0),
    PC850(2),
    WPC1252(16),
    PC852(18),
    PC858(19),
    UNRECOGNIZED(-1);

    companion object {
        internal fun fromRaw(raw: Int): CodePage = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_code_page raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/** pd::escpos::Binarization -- how the raster tier turns grey into dots. Input-only,
 *  used by [Payload.Raster]. */
enum class Binarization(internal val raw: Int) {
    FIXED_THRESHOLD(0),
    FLOYD_STEINBERG(1),
    UNRECOGNIZED(-1);

    companion object {
        internal fun fromRaw(raw: Int): Binarization = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_binarization raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/** pd::CompletionMechanism -- which ordered fence a printer's capability profile
 *  answers with. Read via [Printer.completionMechanism]. */
enum class CompletionMechanism(internal val raw: Int) {
    GS_PAREN_H(0),
    GS_R1(1),
    VENDOR_IDLE(2),
    EPOS_JOB_ID(3),
    STAR_CHECKED_BLOCK(4),
    NONE(5),
    UNRECOGNIZED(-1);

    companion object {
        internal fun fromRaw(raw: Int): CompletionMechanism = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_completion_mechanism raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/**
 * pd::CutVariant -- the cut a profile's completion mechanism actually performs.
 * Mirrored for completeness (pd.h defines the enum and a pd_cut_variant_name() lookup
 * helper), but as of this contract version pd.h has no pd_* accessor that *returns* a
 * pd_cut_variant -- nothing in the current ABI produces one. Kept here, unused, so
 * that if/when a future accessor is added (e.g. as part of the explore()/PrinterInfo
 * surface sketched in docs/api.md §13, which has no C ABI binding at all yet) this
 * wrapper already has the matching enum rather than needing one more mirroring pass.
 */
enum class CutVariant(internal val raw: Int) {
    PARTIAL(0),
    FULL(1),
    NONE(2),
    UNRECOGNIZED(-1);

    companion object {
        internal fun fromRaw(raw: Int): CutVariant = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_cut_variant raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/**
 * pd::ConfidenceGrade -- what *class* of evidence a claim rests on
 * (docs/device-database.md "Confidence grades for every route").
 *
 * Orthogonal to [ConfidenceLevel]: the level says how far up the evidence ladder a job
 * climbed, the grade says what the claim is made of. Done at CUT_PROCESSED on grade A
 * and Done at CUT_PROCESSED on grade D are not the same claim.
 */
enum class ConfidenceGrade(internal val raw: Int, val letter: String) {
    /** GS(H), an ePOS JobID result, a Star checked block. */
    A_JOB_LEVEL_CONFIRMATION(0, "A"),

    /** GS r, a vendor idle query. */
    B_ORDERED_DEVICE_RESPONSE(1, "B"),

    /** DLE EOT, ASB or SNMP taken around the transmission. */
    C_DEVICE_STATUS_AROUND(2, "C"),

    /** A spooler or IPP gateway said completed. */
    D_SPOOLER_COMPLETED(3, "D"),

    /** The write succeeded and nothing else is known. */
    E_TRANSPORT_ONLY(4, "E"),
    UNRECOGNIZED(-1, "?");

    companion object {
        internal fun fromRaw(raw: Int): ConfidenceGrade = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_confidence_grade raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/**
 * pd::CompletionAuthority -- who is actually making the claim a [JobResult] carries.
 *
 * Recorded separately from [ConfidenceGrade] because "completed" from a print server and
 * "completed" from the mechanism that moved the paper are different facts
 * (docs/device-database.md §D).
 */
enum class CompletionAuthority(internal val raw: Int) {
    /** The device that moved the paper said so itself. */
    PHYSICAL_PRINTER(0),
    VENDOR_SPOOLER(1),
    PD_AGENT(2),
    PRINT_SERVER(3),

    /** Nobody said anything; the write succeeded. */
    TRANSPORT_ONLY(4),
    UNRECOGNIZED(-1);

    companion object {
        internal fun fromRaw(raw: Int): CompletionAuthority = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_completion_authority raw value: $raw")
            UNRECOGNIZED
        }
    }
}
