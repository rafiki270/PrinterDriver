package com.printerdriver

import com.printerdriver.internal.NativeBridge
import com.printerdriver.internal.PdLog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.withContext

// M15 -- self-test, auto-detection and LAN discovery (docs/api.md section 15).
//
// The wrapper holds no detection logic. Which provenance column governs a mechanism, what
// a printless probe may claim and how a ticket is laid out are all core questions,
// answered behind pd_self_test / pd_auto_detect / pd_discover. What is here is the shape
// a Kotlin caller expects: data classes, suspend functions off the main thread, and a
// Flow for the two calls that produce a list of candidates.
//
// VERIFICATION STATUS: like the rest of wrappers/android, this file has never been
// compiled -- there is no kotlinc on the machine it was written on. The JNI glue it calls
// IS syntax- and type-checked (scripts/check_kotlin_syntax.sh), and every external it
// names is proven to have a native definition, which is the mismatch that would otherwise
// become an UnsatisfiedLinkError at a till.

/**
 * pd::ProfileSelection -- how the capability profile in force was arrived at.
 *
 * Not a [Provenance]: that says where a claim about one *capability* comes from, and this
 * says where the *profile* came from.
 */
enum class ProfileSelection(internal val raw: Int) {
    /** A device-database entry matched what the device reported about itself. */
    DOCUMENTED(0),

    /** A probe's first-hand findings promoted whatever was selected. */
    PROBED(1),

    /**
     * Neither: the shipped default is the whole truth, which per
     * docs/capability-profiles.md section 8 means UNKNOWN DEVICE rather than ordinary
     * device.
     */
    DEFAULT(2),
    UNRECOGNIZED(-1);

    companion object {
        internal fun fromRaw(raw: Int): ProfileSelection = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_profile_selection raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/** pd::DetectionStatus -- what auto-detection established about one address. */
enum class DetectionStatus(internal val raw: Int) {
    /** The backchannel answered: identification, fences, or both. */
    ANSWERED(0),

    /**
     * The port accepted the connection and said nothing at all. A real finding -- the
     * interface that does not forward status bytes -- and never a failure.
     */
    SILENT(1),

    /**
     * Reachable and deliberately not interrogated. Never render this as "no
     * capabilities": nobody asked.
     */
    UNVERIFIED(2),
    UNREACHABLE(3),
    UNRECOGNIZED(-1);

    companion object {
        internal fun fromRaw(raw: Int): DetectionStatus = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_detection_status raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/**
 * What a device said about itself, and what that is worth.
 *
 * The whole class is evidence, never truth: [trusted] is false until a signal independent
 * of `GS I` agrees with `GS I`, because at least one family ships answering as somebody
 * else's model.
 */
data class DetectedIdentity(
    val vendor: String,
    val model: String,
    val firmware: String,
    val serial: String,
    val trusted: Boolean,
    val confidencePercent: Int,
    val impersonationSuspected: Boolean,
    /** Whether this identification came from the call that produced it, not the cache. */
    val fresh: Boolean
)

/** Roll width and raster width are separate facts; neither is derived from the other. */
data class DetectedMedia(
    val nominalPaperMm: Int,
    val printableWidthDots: Int,
    val charsPerLine: Int,
    val dpi: Int
)

/** The mechanism, the ceiling it allows, who claims it, and what the claim rests on. */
data class DetectedCompletion(
    val mechanism: CompletionMechanism,
    val gradeCeiling: ConfidenceGrade,
    val authority: CompletionAuthority,
    /** The command a support engineer looks up six months later, e.g. `GS(H) fn48`. */
    val method: String,
    val provenance: Provenance
)

/** The drawer facet, classified rather than fired (docs/cash-drawer.md). */
data class DetectedDrawer(
    val present: Boolean,
    /** A drivable method AND an established electrical standard. */
    val kickable: Boolean,
    val portStandard: DrawerPortStandard,
    val voltage: Int
)

/** The detection report -- pd_detection_summary. */
data class DetectionSummary(
    val endpoint: String,
    val identity: DetectedIdentity,
    val profileId: String,
    val selection: ProfileSelection,
    val media: DetectedMedia,
    val completion: DetectedCompletion,
    val drawer: DetectedDrawer,
    /**
     * Everything requested and not delivered, in the words it is printed in --
     * "BARCODE not supported on this path" and its relatives.
     */
    val degradations: List<String>,
    /** One line for a table row. */
    val provenanceSummary: String
)

/** One candidate, classified -- pd_detected_printer. */
data class DetectedPrinter(
    val endpoint: String,
    val host: String,
    val port: Int,
    val status: DetectionStatus,
    val portOpen: Boolean,
    /** True when the classification came from stored findings, not from this call. */
    val fromCache: Boolean,
    /** Whatever `DLE EOT 1` answered during the sweep, as uppercase hex. */
    val dleEotHex: String,
    val summary: DetectionSummary
)

/** One address the LAN sweep found listening -- pd_discovered_device. */
data class DiscoveredDevice(
    val ip: String,
    val port: Int,
    val portOpen: Boolean,
    /**
     * `DLE EOT 1`'s answer as uppercase hex, verbatim and unclassified. Empty means the
     * port accepted the connection and said nothing -- a LAN module that does not forward
     * status bytes, which is a finding and not a failure.
     */
    val dleEotHex: String
) {
    val answered: Boolean get() = dleEotHex.isNotEmpty()
}

/** What one diagnostic ticket established. [result] is the proof. */
data class SelfTestResult(
    val result: JobResult,
    val detection: DetectionSummary,
    val key: String,
    /**
     * The four `GS ( H` characters printed as `V:` and inside the QR, or null on a
     * profile with no wire token to promote.
     */
    val verificationId: String?,
    /** The ticket exactly as it was laid out, one entry per line. */
    val ticketLines: List<String>
)

// --- Unpacking the flat JNI arrays ------------------------------------------------------
//
// NativeBridge carries numbers and strings separately (see its KDoc for the exact field
// orders). Both readers below are cursors over those two streams, so the packing lives in
// one place on each side of the boundary rather than being spelled out at every call.

private class IntCursor(private val values: IntArray) {
    private var index = 0
    fun next(): Int = if (index < values.size) values[index++] else 0
}

private class StringCursor(private val values: Array<String>) {
    private var index = 0
    fun next(): String = if (index < values.size) values[index++] else ""
    fun take(count: Int): List<String> = List(count.coerceAtLeast(0)) { next() }
}

private fun readSummary(ints: IntCursor, strings: StringCursor, degradationCount: Int): DetectionSummary {
    val trusted = ints.next() != 0
    val confidencePercent = ints.next()
    val impersonation = ints.next() != 0
    val fresh = ints.next() != 0
    val selection = ProfileSelection.fromRaw(ints.next())
    val media = DetectedMedia(
        nominalPaperMm = ints.next(),
        printableWidthDots = ints.next(),
        charsPerLine = ints.next(),
        dpi = ints.next()
    )
    val mechanism = CompletionMechanism.fromRaw(ints.next())
    val gradeCeiling = ConfidenceGrade.fromRaw(ints.next())
    val authority = CompletionAuthority.fromRaw(ints.next())
    val provenance = Provenance.fromRaw(ints.next())
    val drawerPresent = ints.next() != 0
    val drawerKickable = ints.next() != 0
    val drawerStandard = DrawerPortStandard.fromRaw(ints.next())
    val drawerVoltage = ints.next()

    val endpoint = strings.next()
    val vendor = strings.next()
    val model = strings.next()
    val firmware = strings.next()
    val serial = strings.next()
    val profileId = strings.next()
    val method = strings.next()
    val provenanceSummary = strings.next()

    return DetectionSummary(
        endpoint = endpoint,
        identity = DetectedIdentity(
            vendor = vendor,
            model = model,
            firmware = firmware,
            serial = serial,
            trusted = trusted,
            confidencePercent = confidencePercent,
            impersonationSuspected = impersonation,
            fresh = fresh
        ),
        profileId = profileId,
        selection = selection,
        media = media,
        completion = DetectedCompletion(
            mechanism = mechanism,
            gradeCeiling = gradeCeiling,
            authority = authority,
            method = method,
            provenance = provenance
        ),
        drawer = DetectedDrawer(
            present = drawerPresent,
            kickable = drawerKickable,
            portStandard = drawerStandard,
            voltage = drawerVoltage
        ),
        degradations = strings.take(degradationCount),
        provenanceSummary = provenanceSummary
    )
}

// --- The public surface -----------------------------------------------------------------

/**
 * Prints ONE diagnostic ticket through the full fenced engine and reports what it
 * established. **This uses paper: the paper is the report.**
 *
 * Identity, profile and how it was selected, media, completion mechanism with its grade
 * ceiling and provenance, the drawer classification, a Czech/Hungarian/Polish charset
 * line, a Code 128 sample and the job's own verification token in the trailer QR. Anything
 * the profile cannot draw is printed as a declared degradation rather than dropped, and
 * repeated in [DetectionSummary.degradations].
 *
 * The printer's OWN built-in self-test (`GS ( A`) is a different document and stays
 * separately reachable through `pdctl test-print`.
 *
 * Suspends on [Dispatchers.IO]; the underlying call blocks until the job is terminal.
 */
suspend fun Printer.selfTest(
    key: String? = null,
    refreshIdentity: Boolean = false,
    probeWithoutPrinting: Boolean = false,
    barcode: Boolean = true,
    barcodeData: String? = null,
    printVerificationId: Boolean = true,
    timeoutMs: Int = 0
): SelfTestResult = withContext(Dispatchers.IO) {
    val values = IntArray(24)
    val strings = NativeBridge.selfTest(
        driver.handle, handle, key, refreshIdentity, probeWithoutPrinting, barcode,
        barcodeData, printVerificationId, timeoutMs, values
    )
    if (strings.isEmpty()) {
        throw PrinterDriverException(driver.lastError)
    }

    val ints = IntCursor(values)
    val outcome = ints.next()
    val confidence = ints.next()
    val reason = ints.next()
    val grade = ints.next()
    val authority = ints.next()
    // The two counts are the last ints, so they have to be read out of the raw array
    // rather than through the cursor, which is still walking the summary.
    val degradationCount = values[22]
    val ticketLineCount = values[23]

    val cursor = StringCursor(strings)
    val method = cursor.next()
    val jobKey = cursor.next()
    val token = cursor.next()
    val summary = readSummary(ints, cursor, degradationCount)
    val ticketLines = cursor.take(ticketLineCount)

    SelfTestResult(
        result = JobResult.fromRaw(outcome, confidence, reason, grade, authority, method),
        detection = summary,
        key = jobKey,
        verificationId = token.ifEmpty { null },
        ticketLines = ticketLines
    )
}

/**
 * Sweeps, identifies and classifies. **Nothing prints and nothing fires.**
 *
 * Discovery (the non-printing `DLE EOT 1` sweep) then multi-signal identification per
 * candidate, then the PRINTLESS subset of the capability probe, respecting the stored
 * findings cache.
 *
 * An ordered fence only means anything when there is print data ahead of it. This call has
 * none, so the fences go out behind an empty buffer: a device that echoes them has proved
 * that its firmware *implements* the command, not that the echo waits for paper to move.
 * The flag is promoted and its provenance is not -- [DetectedCompletion.provenance] stays
 * [Provenance.UNVERIFIED] on a printless answer, and the reason appears in
 * [DetectionSummary.degradations]. Full promotion needs the printing probe or a real job.
 *
 * [endpoints] is an explicit `host` / `host:port` list that skips the sweep entirely -- the
 * path for a caller with a known inventory, and the only one that reports an unreachable
 * address, because it is the only one where somebody named it.
 */
suspend fun PrinterDriver.autoDetect(
    subnetCidr: String? = null,
    endpoints: List<String> = emptyList(),
    port: Int = 0,
    concurrency: Int = 0,
    connectTimeoutMs: Int = 0,
    responseTimeoutMs: Int = 0,
    probeUnknown: Boolean = true
): List<DetectedPrinter> = withContext(Dispatchers.IO) {
    val count = NativeBridge.autoDetect(
        handle, subnetCidr, endpoints.toTypedArray(), port, concurrency,
        connectTimeoutMs, responseTimeoutMs, probeUnknown
    )
    if (count < 0) {
        throw PrinterDriverException(lastError)
    }
    val values = IntArray(22)
    (0 until count).mapNotNull { index ->
        val strings = NativeBridge.detectedAt(handle, index, values)
        if (strings.isEmpty()) {
            null
        } else {
            val ints = IntCursor(values)
            val candidatePort = ints.next()
            val status = DetectionStatus.fromRaw(ints.next())
            val portOpen = ints.next() != 0
            val fromCache = ints.next() != 0
            val degradationCount = values[21]

            val cursor = StringCursor(strings)
            val endpoint = cursor.next()
            val host = cursor.next()
            val dleEotHex = cursor.next()
            DetectedPrinter(
                endpoint = endpoint,
                host = host,
                port = candidatePort,
                status = status,
                portOpen = portOpen,
                fromCache = fromCache,
                dleEotHex = dleEotHex,
                summary = readSummary(ints, cursor, degradationCount)
            )
        }
    }
}

/** [autoDetect] as a Flow, for a UI that fills a table as the sweep goes. */
fun PrinterDriver.autoDetectFlow(
    subnetCidr: String? = null,
    endpoints: List<String> = emptyList(),
    port: Int = 0,
    concurrency: Int = 0,
    connectTimeoutMs: Int = 0,
    responseTimeoutMs: Int = 0,
    probeUnknown: Boolean = true
): Flow<DetectedPrinter> = flow {
    autoDetect(
        subnetCidr, endpoints, port, concurrency, connectTimeoutMs, responseTimeoutMs,
        probeUnknown
    ).forEach { emit(it) }
}.flowOn(Dispatchers.IO)

/**
 * The raw sweep underneath [autoDetect]: is something ESC/POS-shaped listening, and does
 * its backchannel reach me?
 *
 * No identification, no capability probe, no profile -- deciding what a device *is* costs
 * time and belongs to [autoDetect]. The whole write side is `DLE EOT 1` (`10 04 01`),
 * every byte of which is below 0x20 and therefore cannot print on any device, ever.
 */
suspend fun PrinterDriver.discover(
    subnetCidr: String? = null,
    port: Int = 0,
    concurrency: Int = 0,
    connectTimeoutMs: Int = 0,
    responseTimeoutMs: Int = 0,
    probeBackchannel: Boolean = true
): List<DiscoveredDevice> = withContext(Dispatchers.IO) {
    val count = NativeBridge.discover(
        handle, subnetCidr, port, concurrency, connectTimeoutMs, responseTimeoutMs,
        probeBackchannel
    )
    if (count < 0) {
        throw PrinterDriverException(lastError)
    }
    val values = IntArray(2)
    (0 until count).mapNotNull { index ->
        val strings = NativeBridge.discoveredAt(handle, index, values)
        if (strings.isEmpty()) {
            null
        } else {
            DiscoveredDevice(
                ip = strings[0],
                port = values[0],
                portOpen = values[1] != 0,
                dleEotHex = if (strings.size > 1) strings[1] else ""
            )
        }
    }
}

/** [discover] as a Flow. */
fun PrinterDriver.discoverFlow(
    subnetCidr: String? = null,
    port: Int = 0,
    concurrency: Int = 0,
    connectTimeoutMs: Int = 0,
    responseTimeoutMs: Int = 0,
    probeBackchannel: Boolean = true
): Flow<DiscoveredDevice> = flow {
    discover(subnetCidr, port, concurrency, connectTimeoutMs, responseTimeoutMs,
        probeBackchannel).forEach { emit(it) }
}.flowOn(Dispatchers.IO)

/**
 * The local /24 as a CIDR string, or null when it cannot be determined.
 *
 * Found by asking the routing table which local address would be used to reach a remote
 * one; no packet is transmitted.
 */
val PrinterDriver.localSubnet: String?
    get() = NativeBridge.localSubnet(handle).ifEmpty { null }
