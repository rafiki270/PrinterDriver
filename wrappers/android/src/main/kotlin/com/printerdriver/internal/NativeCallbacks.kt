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

/**
 * The embedder half of pd_transport_vtable (pd.h "Custom transports",
 * docs/compatibility-brief.md §25): the platform owns the socket, the core owns the
 * protocol. A plain interface rather than a `fun interface` because the vtable has three
 * operations, but the naming/visibility rules above apply unchanged -- each method is
 * looked up by name and signature at registration time.
 *
 * THREAD CONTRACT, restated from pd.h because getting it wrong here deadlocks the core:
 * all three methods are called BY THE CORE on the owning printer's worker thread, one at
 * a time, never concurrently with each other. None of them may call
 * [NativeBridge.transportFeedBytes] -- the thread they run on is the thread that would
 * have to service the delivery. Received bytes go back in from the implementation's own
 * reader thread instead, which may run concurrently with [write].
 *
 * The registration outlives an individual connection: after a link drop the core calls
 * [connect] again on the same object, so an implementation must be reusable rather than
 * single-shot.
 */
internal interface NativeTransportCallback {
    /** JNI signature "()Z". Open the link; `false` fails the connect attempt (the core
     *  retries on the next job). Must not block indefinitely -- it holds the printer's
     *  worker thread. */
    fun connect(): Boolean

    /**
     * JNI signature "([B)I". Returns the number of bytes actually handed to the link, or
     * a negative value for a hard failure.
     *
     * The count is reported honestly rather than rounded up to [data]`.size`: pd.h makes
     * zero bytes out a known failure and one byte out an Unknown outcome (docs/api.md
     * §4), and which of those an operator sees decides whether they reprint.
     */
    fun write(data: ByteArray): Int

    /** JNI signature "()V". Close the link. Called once per successful [connect]; calling
     *  it again must be harmless. */
    fun close()
}

// --- M16: custom method registration (docs/api.md section 16) -------------------------
//
// The same naming and visibility rules as the interfaces above: each method is found by
// an explicit GetMethodID(name, signature) at registration time, so the names and
// signatures in the KDoc are load-bearing and R8 must not rename them
// (consumer-rules.pro). All of them are invoked BY THE CORE, on a printer's worker
// thread, on the transport reader path, or on whatever thread renders a document. None of
// them may block or call back into the driver.

/**
 * The embedder half of `pd_completion_method`: a vendor idle/ack scheme as a first-class
 * graded completion path.
 */
internal interface NativeCompletionMethodCallback {
    /** JNI signature "(Ljava/lang/String;)[B". The fence bytes for this job's
     *  four-character verification token. A fence the core cannot fit fails that job
     *  Unknown rather than going out truncated. */
    fun fenceBytes(jobToken: String): ByteArray

    /**
     * JNI signature "([B)Ljava/lang/String;". The verdict on the bytes accumulated since
     * the last one, encoded so that a matcher answers in a SINGLE call and nothing has to
     * be remembered between two (two concurrent renders would race):
     *
     *  - `null` — NotMine: not this mechanism's bytes; the core drops its buffer.
     *  - `""` — NeedMore: an answer may be forming; keep buffering.
     *  - anything else — Matched, and the string is the correlation token.
     */
    fun match(data: ByteArray): String?
}

/** The embedder half of `pd_probe_step`. */
internal fun interface NativeProbeStepCallback {
    /** JNI signature "([B)Ljava/lang/String;". `null` means the device did not answer
     *  this step at all; any string means it did, and is the finding's label. */
    fun classify(response: ByteArray): String?
}

/** The embedder half of `pd_block_handler`. */
internal fun interface NativeBlockHandlerCallback {
    /**
     * JNI signature "(Ljava/lang/String;Ljava/lang/String;)[B". One tagged answer, for the
     * same single-call reason as [NativeCompletionMethodCallback.match]:
     *
     *  - first byte `1` — the rest is raw ESC/POS ops;
     *  - first byte `0` — the rest is a UTF-8 degradation reason, reported exactly like a
     *    built-in block's;
     *  - `null` or empty — a degradation with no reason given.
     */
    fun render(blockJson: String, profileJson: String): ByteArray?
}

/** The embedder half of `pd_formatter`. */
internal fun interface NativeFormatterCallback {
    /** JNI signature
     *  "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;".
     *  `null` declines, and the built-in formatter table answers instead. */
    fun format(value: String, args: String, locale: String): String?
}

/** The embedder half of `pd_drawer_kick_reg`. */
internal interface NativeDrawerKickCallback {
    /** JNI signature "(II)[B". The pulse bytes for (channel, pulse milliseconds). */
    fun kickBytes(channel: Int, pulseMs: Int): ByteArray

    /** JNI signature "()[B". The bytes that ask for the switch state. Only called when the
     *  registration declared a readable switch. */
    fun statusRequest(): ByteArray

    /** JNI signature "([B)I". The pin level: -1 unknown, 0 low, 1 high — pd.h's
     *  PD_UNKNOWN / PD_FALSE / PD_TRUE. A level nobody could read is unknown, never
     *  "closed". */
    fun statusParse(response: ByteArray): Int
}
