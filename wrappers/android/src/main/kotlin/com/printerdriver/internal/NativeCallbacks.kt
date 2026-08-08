package com.printerdriver.internal

// SAM interfaces the native layer calls back into via JNI, by explicit
// env->GetMethodID(runtimeClass, "<name>", "<signature>") lookups against the exact
// names/signatures below -- see src/main/cpp/printerdriver_jni.cpp's own header
// comment for the derivation of each JNI signature string. Consequences that follow
// directly from that lookup mechanism:
//
//  - The containing interface is `internal` (these are not part of the public API --
//    Kotlin callers use Flow<JobEvent>/Flow<DeviceEvent> or the send() closure sugar,
//    never these directly), but the single abstract method on each is deliberately
//    left at the DEFAULT (public) visibility rather than also marked `internal`.
//    Kotlin mangles the compiled names of `internal` functions/methods with a
//    module-specific suffix so that same-named internal members in different Gradle
//    modules cannot collide at the ABI level; that mangling would change the method
//    name GetMethodID is asked to look up out from under it. Restricting reachability
//    at the interface level is enough for encapsulation and does not trigger this.
//  - R8/ProGuard must not rename these methods either, for the same reason -- see
//    consumer-rules.pro's `-keep` rules for all three interfaces and their
//    implementers.

internal fun interface NativeJobEventCallback {
    /** JNI signature "(IIZIJ)V". Mirrors pd_job_event field-for-field: raw
     *  pd_job_state, raw pd_confidence_level, `has_reason`, raw pd_failure_reason
     *  (meaningless when `hasReason` is false, same as pd.h's own `has_reason`
     *  contract), and the steady-clock `monotonic_ms`. */
    fun onEvent(state: Int, confidence: Int, hasReason: Boolean, reason: Int, monotonicMs: Long)
}

internal fun interface NativeDeviceEventCallback {
    /** JNI signature "(I)V". Raw pd_device_event. */
    fun onEvent(event: Int)
}

internal fun interface NativeLogCallback {
    /** JNI signature "(Ljava/lang/String;)V". Bridges pd_log_cb. */
    fun onMessage(message: String)
}
