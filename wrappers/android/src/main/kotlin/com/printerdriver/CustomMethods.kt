package com.printerdriver

import com.printerdriver.internal.NativeBlockHandlerCallback
import com.printerdriver.internal.NativeBridge
import com.printerdriver.internal.NativeCompletionMethodCallback
import com.printerdriver.internal.NativeDrawerKickCallback
import com.printerdriver.internal.NativeFormatterCallback
import com.printerdriver.internal.NativeProbeStepCallback

// M16 -- custom method registration (docs/api.md section 16).
//
// Five ways to extend the SDK at runtime without forking it: a vendor idle/ack scheme
// becomes a first-class graded completion path, a house-specific fingerprint joins
// probe/autoDetect, a new DSL block kind renders through the ordinary pipeline, a template
// gains a formatter, and a vendor drawer method fills DrawerKickMethod.VENDOR. All are per
// driver, all are keyed by namespaced ids ("acme.x-idle"), and everything a registration
// claims -- a grade, an authority, a method name -- is attributed to it BY ID in the job
// result and in `pdctl verify`. That attribution is the only reason extending the
// honesty-critical part of this SDK is allowed at all.
//
// THREAD CONTRACT, restated from pd.h because getting it wrong here stalls a receipt:
// every lambda below is called BY THE CORE -- a fence and matcher on the printer's worker
// thread and its transport reader path, a probe classifier on the worker driving the
// probe, a formatter or block renderer on whatever thread lays out the document. None of
// them may block, and none may call back into the driver. Each is retained for the life of
// the driver, because pd.h has no unregister call.
//
// VERIFICATION STATUS: like the rest of wrappers/android, this file has never been
// compiled -- there is no kotlinc on the machine it was written on. The JNI glue it calls
// IS syntax- and type-checked (scripts/check_kotlin_syntax.sh), and every external it
// names is proven to have a native definition.

/** A custom matcher's verdict on the printer-to-host bytes it was handed. */
sealed class CompletionMatch {
    /** A fence answer carrying [token]; confirms the job exactly like `GS ( H`. */
    data class Matched(val token: String) : CompletionMatch()

    /** Not this mechanism's bytes; the core drops its matcher buffer and reads on. */
    data object NotMine : CompletionMatch()

    /** An answer may be forming but is incomplete; the core keeps buffering. */
    data object NeedMore : CompletionMatch()
}

/** `pd_match_kind` -- the verdict kinds, for a diagnostic that needs to name one. */
enum class CompletionMatchKind(internal val raw: Int) {
    MATCHED(0),
    NOT_MINE(1),
    NEED_MORE(2),
}

/** What a custom probe step concluded about the answer it was handed. */
data class ProbeFinding(
    /** Whether the device replied to this step at all. */
    val answered: Boolean,
    /** A short classification, surfaced in the findings summary; truncated at 63 bytes. */
    val label: String = ""
)

/** What a custom block handler made of a block: ops, or a declared degradation. */
sealed class BlockRendering {
    /** Raw ESC/POS ops, rendered through the ordinary pipeline. */
    data class Ops(val bytes: ByteArray) : BlockRendering() {
        override fun equals(other: Any?): Boolean =
            this === other || (other is Ops && bytes.contentEquals(other.bytes))

        override fun hashCode(): Int = bytes.contentHashCode()
    }

    /** This block could not be drawn, and here is the one line saying so. Reported like a
     *  built-in block's degradation -- never dropped in silence. */
    data class Degraded(val reason: String) : BlockRendering()
}

/**
 * Registers a custom completion mechanism (docs/api.md section 16).
 *
 * Bind a printer to it by attaching with the profile id `"vendoridle:<id>"`, e.g.
 * `"vendoridle:acme.x-idle"`, which resolves to a generic ESC/POS profile whose completion
 * is this method. The engine sends [fenceBytes] behind the payload and routes the printer's
 * response stream through [matcher]; a [CompletionMatch.Matched] confirms the job exactly
 * like `GS ( H`, with the same per-job token map and the same resolvable verification
 * identifier.
 *
 * @param id namespaced, e.g. `acme.x-idle`.
 * @param grade what a confirmed completion on this method claims.
 * @param authority who makes that claim.
 * @param methodName shown in the result and in `pdctl verify`; null uses [id].
 * @param fenceBytes the fence for a job's four-character verification token.
 * @param matcher the verdict on the bytes accumulated since the last one.
 * @throws PrinterDriverException on a bad or duplicate id, or a record the core refused.
 */
fun PrinterDriver.registerCompletionMethod(
    id: String,
    grade: ConfidenceGrade = ConfidenceGrade.A_JOB_LEVEL_CONFIRMATION,
    authority: CompletionAuthority = CompletionAuthority.PHYSICAL_PRINTER,
    methodName: String? = null,
    fenceBytes: (jobToken: String) -> ByteArray,
    matcher: (data: ByteArray) -> CompletionMatch
) {
    checkOpen()
    val callback = object : NativeCompletionMethodCallback {
        override fun fenceBytes(jobToken: String): ByteArray = fenceBytes.invoke(jobToken)

        // The wire encoding of a verdict (see NativeCompletionMethodCallback.match):
        // null is NotMine, "" is NeedMore, anything else is the matched token.
        override fun match(data: ByteArray): String? = when (val verdict = matcher(data)) {
            is CompletionMatch.Matched -> verdict.token.ifEmpty { "" }
            CompletionMatch.NotMine -> null
            CompletionMatch.NeedMore -> ""
        }
    }
    val ok = NativeBridge.registerCompletionMethod(
        handle, id, methodName ?: id, grade.raw, authority.raw, callback
    )
    if (!ok) {
        throw PrinterDriverException("pd_register_completion_method failed: $lastError")
    }
}

/**
 * Registers an extra fingerprinting step for `probe` and [autoDetect].
 *
 * [requestBytes] MUST be non-printing -- no 0x20-0x7E run, no line feed. A printing step is
 * refused here rather than at a venue, because auto-detection must never cost somebody a
 * roll of paper.
 *
 * @throws PrinterDriverException on a bad or duplicate id, or a request that could print.
 */
fun PrinterDriver.registerProbeStep(
    id: String,
    requestBytes: ByteArray,
    classify: (response: ByteArray) -> ProbeFinding
) {
    checkOpen()
    val callback = NativeProbeStepCallback { response ->
        val finding = classify(response)
        if (finding.answered) finding.label else null
    }
    if (!NativeBridge.registerProbeStep(handle, id, requestBytes, callback)) {
        throw PrinterDriverException("pd_register_probe_step failed: $lastError")
    }
}

/**
 * Registers a renderer for a new DSL block kind. A handler registered for a kind always
 * owns it: unknown kinds otherwise degrade, and this intercepts first.
 *
 * Reached through [Printer.renderDocument] and [Printer.printDocument] -- the receipt-DSL
 * entry points -- not through [Printer.print], whose payload tiers have no block kinds.
 *
 * @param kind the block object key that selects this handler.
 * @param render receives the block object as JSON and a small JSON of the render profile
 *   facts (`{"width_dots":576,"barcode":true,...}`).
 * @throws PrinterDriverException on a bad or duplicate kind.
 */
fun PrinterDriver.registerBlockHandler(
    kind: String,
    render: (blockJson: String, profileJson: String) -> BlockRendering
) {
    checkOpen()
    val callback = NativeBlockHandlerCallback { blockJson, profileJson ->
        // One tagged answer, so two concurrent renders cannot pick up each other's
        // reason: first byte 1 -> ops, first byte 0 -> a UTF-8 degradation reason.
        when (val rendering = render(blockJson, profileJson)) {
            is BlockRendering.Ops -> byteArrayOf(1) + rendering.bytes
            is BlockRendering.Degraded ->
                byteArrayOf(0) + rendering.reason.toByteArray(Charsets.UTF_8)
        }
    }
    if (!NativeBridge.registerBlockHandler(handle, kind, callback)) {
        throw PrinterDriverException("pd_register_block_handler failed: $lastError")
    }
}

/**
 * Registers a template formatter, backing `{{ v | name:args }}` and checked before the
 * built-in table. Returning null from [format] declines and falls through to the built-ins.
 *
 * Consulted wherever a template is bound: [Printer.renderDocument],
 * [Printer.printDocument] and this driver's self-test tickets.
 *
 * @throws PrinterDriverException on a bad or duplicate name.
 */
fun PrinterDriver.registerFormatter(
    name: String,
    format: (value: String, args: String, locale: String) -> String?
) {
    checkOpen()
    val callback = NativeFormatterCallback { value, args, locale -> format(value, args, locale) }
    if (!NativeBridge.registerFormatter(handle, name, callback)) {
        throw PrinterDriverException("pd_register_formatter failed: $lastError")
    }
}

/**
 * Registers a vendor drawer-kick method, filling [DrawerKickMethod.VENDOR] for a profile
 * (docs/cash-drawer.md).
 *
 * [statusRequest] and [statusParse] go together: leaving both null means the method has no
 * readable switch, so a kick reports [DrawerState.KICK_SENT_UNVERIFIED] rather than a
 * verified open. That is the honest answer, not a weak success.
 *
 * @param kickBytes the pulse bytes for (channel, pulse milliseconds).
 * @param statusRequest the bytes that ask for the switch state.
 * @param statusParse the pin level a reply carries: true high, false low, null unreadable.
 * @throws PrinterDriverException on a bad or duplicate id.
 */
fun PrinterDriver.registerDrawerKick(
    id: String,
    kickBytes: (channel: Int, pulseMs: Int) -> ByteArray,
    statusRequest: (() -> ByteArray)? = null,
    statusParse: ((response: ByteArray) -> Boolean?)? = null
) {
    checkOpen()
    val readableSwitch = statusRequest != null && statusParse != null
    val callback = object : NativeDrawerKickCallback {
        override fun kickBytes(channel: Int, pulseMs: Int): ByteArray =
            kickBytes.invoke(channel, pulseMs)

        override fun statusRequest(): ByteArray = statusRequest?.invoke() ?: ByteArray(0)

        override fun statusParse(response: ByteArray): Int =
            when (statusParse?.invoke(response)) {
                true -> 1
                false -> 0
                null -> -1
            }
    }
    if (!NativeBridge.registerDrawerKick(handle, id, readableSwitch, callback)) {
        throw PrinterDriverException("pd_register_drawer_kick failed: $lastError")
    }
}
