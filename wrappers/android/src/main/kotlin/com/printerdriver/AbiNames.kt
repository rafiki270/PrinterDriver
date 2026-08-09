package com.printerdriver

import com.printerdriver.internal.AbiEnum
import com.printerdriver.internal.NativeBridge

// M17 -- the core's own spelling of every mirrored enum (docs/api.md section 17).
//
// Not `name`. A Kotlin member name is THIS wrapper's spelling; an ABI name is the core's,
// and the core's is what the journal records, what pdctl prints and what a support
// engineer greps for six months later. Rendering a diagnostic with one and then searching
// the journal for the other is a wasted afternoon, so every mirrored enum can be asked.
//
// Each property is one JNI call into pd_<enum>_name; nothing is cached, because these are
// diagnostics and a stale mirror is the failure mode being avoided.

/** `pd_job_state_name`. */
val JobState.abiName: String get() = NativeBridge.abiEnumName(AbiEnum.JOB_STATE, raw)

/** `pd_confidence_level_name`. */
val ConfidenceLevel.abiName: String
    get() = NativeBridge.abiEnumName(AbiEnum.CONFIDENCE_LEVEL, raw)

/** `pd_device_event_name`. */
val DeviceEvent.abiName: String get() = NativeBridge.abiEnumName(AbiEnum.DEVICE_EVENT, raw)

/** `pd_failure_reason_name`. */
val FailureReason.abiName: String
    get() = NativeBridge.abiEnumName(AbiEnum.FAILURE_REASON, raw)

/** `pd_confidence_grade_name`. */
val ConfidenceGrade.abiName: String
    get() = NativeBridge.abiEnumName(AbiEnum.CONFIDENCE_GRADE, raw)

/**
 * `pd_confidence_grade_letter` -- "A+", "A".."E", the letter a report tabulates.
 *
 * [ConfidenceGrade.letter] is the same letter mirrored in Kotlin, for a UI that must not
 * pay a JNI call per row. This one is the core's own answer, and the two are asserted
 * equal by the enum-bridge test; when a report is the evidence, prefer this.
 */
val ConfidenceGrade.abiLetter: String
    get() = NativeBridge.abiEnumName(AbiEnum.CONFIDENCE_GRADE_LETTER, raw)

/** `pd_completion_authority_name`. */
val CompletionAuthority.abiName: String
    get() = NativeBridge.abiEnumName(AbiEnum.COMPLETION_AUTHORITY, raw)

/** `pd_provenance_name`. */
val Provenance.abiName: String get() = NativeBridge.abiEnumName(AbiEnum.PROVENANCE, raw)

/** `pd_command_language_name`. */
val CommandLanguage.abiName: String
    get() = NativeBridge.abiEnumName(AbiEnum.COMMAND_LANGUAGE, raw)

/** `pd_completion_mechanism_name`. */
val CompletionMechanism.abiName: String
    get() = NativeBridge.abiEnumName(AbiEnum.COMPLETION_MECHANISM, raw)

/** `pd_cut_variant_name`. */
val CutVariant.abiName: String get() = NativeBridge.abiEnumName(AbiEnum.CUT_VARIANT, raw)

/** `pd_drawer_state_name`. */
val DrawerState.abiName: String get() = NativeBridge.abiEnumName(AbiEnum.DRAWER_STATE, raw)

/** `pd_drawer_port_standard_name`. */
val DrawerPortStandard.abiName: String
    get() = NativeBridge.abiEnumName(AbiEnum.DRAWER_PORT_STANDARD, raw)

/** `pd_drawer_kick_method_name`. */
val DrawerKickMethod.abiName: String
    get() = NativeBridge.abiEnumName(AbiEnum.DRAWER_KICK_METHOD, raw)

/** `pd_drawer_status_method_name`. */
val DrawerStatusMethod.abiName: String
    get() = NativeBridge.abiEnumName(AbiEnum.DRAWER_STATUS_METHOD, raw)

/** `pd_profile_selection_name`. */
val ProfileSelection.abiName: String
    get() = NativeBridge.abiEnumName(AbiEnum.PROFILE_SELECTION, raw)

/** `pd_detection_status_name`. */
val DetectionStatus.abiName: String
    get() = NativeBridge.abiEnumName(AbiEnum.DETECTION_STATUS, raw)

/** `pd_drain_order_name`. */
val DrainOrder.abiName: String get() = NativeBridge.abiEnumName(AbiEnum.DRAIN_ORDER, raw)

/** `pd_match_kind_name` -- a custom matcher's verdict (docs/api.md section 16). */
val CompletionMatchKind.abiName: String
    get() = NativeBridge.abiEnumName(AbiEnum.MATCH_KIND, raw)

/**
 * `pd_job_outcome_name` -- the core's spelling of which of the three this result is.
 *
 * The tri-state is modelled as sealed subclasses rather than as an enum, because `when`
 * over them is exhaustive and a wrapper-side enum could never be; this is the bridge back
 * to the ABI's own name for the same fact.
 */
val JobResult.abiOutcomeName: String
    get() = NativeBridge.abiEnumName(
        AbiEnum.JOB_OUTCOME,
        when (this) {
            is JobResult.Done -> 0
            is JobResult.Failed -> 1
            is JobResult.Unknown -> 2
        }
    )

/**
 * `pd_payload_kind_name` -- the core's spelling of which tier this payload is
 * (docs/api.md section 3). Sealed for the same reason [JobResult] is.
 */
val Payload.abiKindName: String
    get() = NativeBridge.abiEnumName(
        AbiEnum.PAYLOAD_KIND,
        when (this) {
            is Payload.Raster -> 0
            is Payload.Document -> 1
            is Payload.Raw -> 2
        }
    )
