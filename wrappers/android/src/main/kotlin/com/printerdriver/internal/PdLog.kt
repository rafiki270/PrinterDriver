package com.printerdriver.internal

import android.util.Log

/**
 * One log tag for the whole wrapper, so every "this shouldn't happen but let's not
 * crash" path (unrecognized raw enum values from a version-skewed native library,
 * dropped callbacks during JVM shutdown) is greppable in logcat under one name.
 */
internal object PdLog {
    private const val TAG = "PrinterDriver"

    fun w(message: String) {
        Log.w(TAG, message)
    }
}

/**
 * Which `pd_*_name` function [NativeBridge.abiEnumName] should call.
 *
 * An implementation detail of the `abiName` properties in AbiNames.kt: one JNI entry
 * point serves every mirrored enum, and this is the selector it switches on. The values
 * MUST match the `switch` in Java_com_printerdriver_internal_NativeBridge_abiEnumName.
 */
internal object AbiEnum {
    const val JOB_STATE = 0
    const val CONFIDENCE_LEVEL = 1
    const val DEVICE_EVENT = 2
    const val FAILURE_REASON = 3
    const val JOB_OUTCOME = 4
    const val CONFIDENCE_GRADE = 5
    const val COMPLETION_AUTHORITY = 6
    const val PROVENANCE = 7
    const val COMMAND_LANGUAGE = 8
    const val PAYLOAD_KIND = 9
    const val COMPLETION_MECHANISM = 10
    const val CUT_VARIANT = 11
    const val DRAWER_STATE = 12
    const val DRAWER_PORT_STANDARD = 13
    const val DRAWER_KICK_METHOD = 14
    const val DRAWER_STATUS_METHOD = 15
    const val PROFILE_SELECTION = 16
    const val DETECTION_STATUS = 17
    const val DRAIN_ORDER = 18
    const val MATCH_KIND = 19

    /** Not a member name: the letter a report tabulates ("A+", "A".."E"). */
    const val CONFIDENCE_GRADE_LETTER = 20
}
