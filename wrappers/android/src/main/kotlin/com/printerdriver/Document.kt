package com.printerdriver

import com.printerdriver.internal.NativeBridge
import com.printerdriver.internal.PdLog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject

// M19 -- the receipt DSL through the wrapper (docs/receipt-dsl.md, docs/api.md §3).
//
// No rendering logic lives here and no template engine. Which blocks a profile can draw,
// what a missing model path does, how `meta.cut` reaches a job: all of it is in the core
// behind pd_render_document and pd_print_document_json. What is here is the shape a Kotlin
// caller expects -- data classes, a typed report instead of an opaque string, suspend
// functions, and a JSONObject overload for the JSON an Android app already has.

/**
 * What kind of departure from the document was declared -- `pd_report_kind`.
 *
 * Every one of these RENDERS: the receipt still prints and the entry says what it lost.
 * A declared degradation is not a failure, but it is never silent either.
 */
enum class ReportKind(internal val raw: Int) {
    /** `{{order.note}}` with no such path in the model. */
    MISSING_PATH(0),

    /** `{{v|frobnicate}}`. */
    UNKNOWN_FORMATTER(1),

    /** `{{v|number:2}}` where `v` is an object. */
    UNFORMATTABLE_VALUE(2),

    /** An unterminated `{{`, an empty path -- and the three hard failures that stop bytes
     *  being produced at all: JSON that is not JSON, a structure that is not a document,
     *  and a template submitted with no model. */
    MALFORMED_TEMPLATE(3),

    UNKNOWN_STYLE(4),

    /** An `extends` chain that loops. */
    STYLE_CYCLE(5),

    /** Italic, `rotate90`, a raster font on the hardware path. */
    UNSUPPORTED_STYLE(6),

    /** A symbology this build cannot draw, or a block this profile has no hardware for. */
    UNSUPPORTED_BLOCK(7),

    MISSING_IMAGE(8),

    /** Content clipped to fit the media width. */
    TRUNCATED(9),

    /** `each` over a missing or non-array path. */
    EMPTY_ITERATION(10),

    /** An IANA zone name: this core ships no timezone database. */
    UNSUPPORTED_TIMEZONE(11),

    /** A `raw` block carrying a cut or a status command -- the core owns job termination. */
    RAW_FRAMING_RISK(12),

    /** Everything else worth telling the operator. */
    NOTE(13),

    /** Not a real pd_report_kind value -- see the note at the top of Enums.kt. */
    UNRECOGNIZED(-1);

    /** The core's own spelling, from `pd_report_kind_name`. */
    val abiName: String get() = NativeBridge.reportKindName(raw)

    companion object {
        internal fun fromRaw(raw: Int): ReportKind = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_report_kind raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/** How an entry's content reached the paper, if it did -- `pd_render_path`. */
enum class RenderPath(internal val raw: Int) {
    /** Produced by ESC/POS commands. */
    HARDWARE(0),

    /** Produced as an image. */
    RASTER(1),

    /** Requested, and deliberately absent from the output. */
    NOT_RENDERED(2),

    /** Not a real pd_render_path value -- see the note at the top of Enums.kt. */
    UNRECOGNIZED(-1);

    /** The core's own spelling, from `pd_render_path_name`. */
    val abiName: String get() = NativeBridge.renderPathName(raw)

    companion object {
        internal fun fromRaw(raw: Int): RenderPath = entries.firstOrNull { it.raw == raw } ?: run {
            PdLog.w("Unrecognized pd_render_path raw value: $raw")
            UNRECOGNIZED
        }
    }
}

/**
 * One declared degradation -- `pd_report_entry`.
 *
 * @property block where it happened, as a path into the document: `blocks[3].cells[1]`,
 *   `meta.tz`, or `document` for one that belongs to no block.
 * @property requested what the document asked for, in the words it is printed in.
 * @property delivered what the paper got. Often `omitted`.
 * @property note why, when the pair above does not say it all. Empty when it does.
 */
data class ReportEntry(
    val kind: ReportKind,
    val block: String,
    val requested: String,
    val delivered: String,
    val path: RenderPath,
    val note: String
) {
    override fun toString(): String {
        val because = if (note.isEmpty()) "" else " - $note"
        return "$block  ${kind.abiName}: requested \"$requested\", delivered " +
            "\"$delivered\" [${path.abiName}]$because"
    }
}

/**
 * What a document asks the engine for, read back from its `meta` (docs/receipt-dsl.md
 * "Cut control" and "Margins").
 *
 * Reported by [RenderedDocument] and applied by [Printer.printDocument] under that
 * document's precedence: what the caller put in [JobOptions] wins, this fills in what the
 * caller left alone, and the printer's profile answers what neither said.
 *
 * @property cut `null` when the document asked for no particular cut.
 * @property topFeedDots blank paper before the first content line; 0 when unstated.
 * @property bottomFeedDots the *total* whitespace between the last content and the cut.
 *   The engine feeds `max(the profile's blade clearance, this)`, so no document can clip
 *   its own trailer.
 */
data class DocumentMeta(
    val cut: Cut?,
    val topFeedDots: Int,
    val bottomFeedDots: Int
)

/**
 * A rendered receipt-DSL document: the bytes a printer would receive, and everything that
 * was declared along the way.
 *
 * @property bytes the ESC/POS the renderer produced. Never the whole job: the engine adds
 *   its own initialise, trailing feed, cut and completion fence around this.
 * @property report empty when every block rendered exactly as written.
 */
data class RenderedDocument(
    val bytes: ByteArray,
    val codePage: CodePage,
    val meta: DocumentMeta,
    val report: List<ReportEntry>
) {
    // ByteArray in a data class needs both by hand: the generated ones compare identity.
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is RenderedDocument) return false
        return bytes.contentEquals(other.bytes) && codePage == other.codePage &&
            meta == other.meta && report == other.report
    }

    override fun hashCode(): Int {
        var result = bytes.contentHashCode()
        result = 31 * result + codePage.hashCode()
        result = 31 * result + meta.hashCode()
        result = 31 * result + report.hashCode()
        return result
    }
}

/**
 * Options for [Printer.renderDocument]. Every default defers to the printer and to the
 * document.
 *
 * @property widthDots 0 lays the document out for this printer's own configured width,
 *   which is what the engine rasterizes to. Anything else previews a different media width.
 * @property cutClearanceDots extra whitespace before a **mid-document** `cut` block. The
 *   renderer feeds `max(the profile's blade clearance, this)`: more is always granted,
 *   less never.
 * @property maxRowsPerBand 0 means 1024 -- the Epson tall-image split, for `image` blocks.
 * @property locale overrides the document's own `meta.locale`; `null` defers to it.
 * @property currency overrides `meta.currency`.
 * @property timeZone overrides `meta.tz`. A fixed offset such as `+02:00`; an IANA name is
 *   reported as a [ReportKind.UNSUPPORTED_TIMEZONE] degradation, because a core that ships
 *   no dependencies ships no timezone database.
 */
data class RenderOptions(
    val widthDots: Int = 0,
    val cutClearanceDots: Int = 0,
    val maxRowsPerBand: Int = 0,
    val locale: String? = null,
    val currency: String? = null,
    val timeZone: String? = null
)

/**
 * Renders a receipt-DSL document against this printer's capability profile.
 *
 * **Nothing prints and no job exists.** This is the preview path: the bytes a printer would
 * receive, plus every degradation the profile forced, before any paper is committed.
 *
 * @param documentJson a document or a template (docs/receipt-dsl.md).
 * @param modelJson the parameter model bound into a template -- `{{path.to.value}}`,
 *   `each`, `if`. `null` for a document that carries its own content; a template handed no
 *   model is refused rather than printed, because a receipt full of `{{order.total}}` is
 *   worse than no receipt -- it looks like one.
 * @throws PrinterDriverException when the JSON is not JSON, the structure is not a
 *   document, or a template arrived with no model. Everything softer is a [ReportEntry] in
 *   [RenderedDocument.report] and still renders.
 */
suspend fun Printer.renderDocument(
    documentJson: String,
    modelJson: String? = null,
    options: RenderOptions = RenderOptions()
): RenderedDocument = withContext(Dispatchers.IO) {
    driver.checkOpen()
    val values = IntArray(7)
    val bytes = NativeBridge.renderDslDocument(
        driver.handle, handle, documentJson, modelJson,
        options.widthDots, options.cutClearanceDots, options.maxRowsPerBand,
        options.locale, options.currency, options.timeZone, values
    )
    // The report is read on both paths: a caller that cannot print gets the same list it
    // would show for a degradation.
    val report = driver.renderReport()
    if (values[0] != 1) {
        throw PrinterDriverException(
            (driver.lastError + "\n" + report.joinToString("\n")).trim()
        )
    }
    RenderedDocument(
        bytes = bytes,
        codePage = CodePage.fromRaw(values[1]),
        meta = DocumentMeta(
            cut = if (values[2] != 0) Cut.fromRaw(values[3]) else null,
            topFeedDots = values[4],
            bottomFeedDots = values[5]
        ),
        report = report
    )
}

/** [renderDocument] taking the JSON an Android app already has. */
suspend fun Printer.renderDocument(
    document: JSONObject,
    model: JSONObject? = null,
    options: RenderOptions = RenderOptions()
): RenderedDocument = renderDocument(document.toString(), model?.toString(), options)

/**
 * Renders a receipt-DSL document and prints it.
 *
 * The job is an ordinary [PrintJob]: same worker, same preflight, same completion fence,
 * same confidence grading, same idempotency-key dedupe as [Printer.print]. There is no
 * second engine -- a template job proves exactly what a raster job proves.
 *
 * The document's `meta` reaches the job through [options]: a default [JobOptions] lets the
 * document decide its own cut and margins, and any field set explicitly wins over it.
 *
 * @return the ordinary job, carrying what the renderer declared in [PrintJob.renderReport].
 *   That is worth reading on the success path: a receipt that printed with a dropped
 *   barcode is a receipt that printed.
 * @throws PrinterDriverException when there are no bytes to send. Nothing is submitted in
 *   that case -- a malformed document never becomes a blank receipt.
 */
suspend fun Printer.printDocument(
    documentJson: String,
    modelJson: String? = null,
    options: JobOptions = JobOptions()
): PrintJob = withContext(Dispatchers.IO) {
    driver.checkOpen()
    val jobHandle = NativeBridge.printDslDocument(
        driver.handle, handle, documentJson, modelJson,
        options.key, options.cut.raw, options.openDrawer, options.preflight.raw,
        options.timeoutMs, options.topFeedDots, options.bottomFeedDots,
        !options.printVerificationId
    )
    val report = driver.renderReport()
    if (jobHandle == 0L) {
        throw PrinterDriverException(
            ("pd_print_document_json failed: " + driver.lastError + "\n" +
                report.joinToString("\n")).trim()
        )
    }
    PrintJob(driver, jobHandle, report)
}

/** [printDocument] taking the JSON an Android app already has. */
suspend fun Printer.printDocument(
    document: JSONObject,
    model: JSONObject? = null,
    options: JobOptions = JobOptions()
): PrintJob = printDocument(document.toString(), model?.toString(), options)

/**
 * The last document render's report, pulled out through the ABI's index reader.
 *
 * Read immediately after the call that produced it: pd.h owns these strings only until the
 * next render on this driver, and this is where they stop being borrowed. Callers normally
 * reach it through [RenderedDocument.report] or [PrintJob.renderReport], which do exactly
 * that at the right moment.
 */
fun PrinterDriver.renderReport(): List<ReportEntry> {
    checkOpen()
    val count = NativeBridge.renderReportCount(handle)
    if (count <= 0) return emptyList()
    val values = IntArray(2)
    val entries = ArrayList<ReportEntry>(count)
    for (index in 0 until count) {
        val strings = NativeBridge.renderReportAt(handle, index, values)
        if (strings.size < 4) continue
        entries += ReportEntry(
            kind = ReportKind.fromRaw(values[0]),
            block = strings[0],
            requested = strings[1],
            delivered = strings[2],
            path = RenderPath.fromRaw(values[1]),
            note = strings[3]
        )
    }
    return entries
}
