package com.printerdriver

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * M19 -- the render report's mapping onto pd.h, and the shape of the values that carry it
 * (docs/receipt-dsl.md).
 *
 * Deliberately does not touch [com.printerdriver.internal.NativeBridge] (no
 * `System.loadLibrary` anywhere in this file), so it runs as a plain JVM unit test with no
 * native .so and no device -- the same rule every other test in this directory follows.
 * The end-to-end behaviour of `renderDocument` / `printDocument` is proven at the C level
 * in capi/tests/test_capi.c and in the Swift, Dart and .NET suites, which do have a native
 * library to bind; what CAN be checked here is that a raw int arriving from the ABI lands
 * on the member pd.h names for it, and that an unknown one degrades instead of guessing.
 *
 * UNVERIFIED: written but never executed -- this scaffold was authored on a host with no
 * JVM/Gradle/Android SDK. See README.md "Verification status".
 */
class DocumentReportTest {

    @Test
    fun `ReportKind fromRaw matches every pd_report_kind value from pd h`() {
        assertEquals(ReportKind.MISSING_PATH, ReportKind.fromRaw(0))
        assertEquals(ReportKind.UNKNOWN_FORMATTER, ReportKind.fromRaw(1))
        assertEquals(ReportKind.UNFORMATTABLE_VALUE, ReportKind.fromRaw(2))
        assertEquals(ReportKind.MALFORMED_TEMPLATE, ReportKind.fromRaw(3))
        assertEquals(ReportKind.UNKNOWN_STYLE, ReportKind.fromRaw(4))
        assertEquals(ReportKind.STYLE_CYCLE, ReportKind.fromRaw(5))
        assertEquals(ReportKind.UNSUPPORTED_STYLE, ReportKind.fromRaw(6))
        assertEquals(ReportKind.UNSUPPORTED_BLOCK, ReportKind.fromRaw(7))
        assertEquals(ReportKind.MISSING_IMAGE, ReportKind.fromRaw(8))
        assertEquals(ReportKind.TRUNCATED, ReportKind.fromRaw(9))
        assertEquals(ReportKind.EMPTY_ITERATION, ReportKind.fromRaw(10))
        assertEquals(ReportKind.UNSUPPORTED_TIMEZONE, ReportKind.fromRaw(11))
        assertEquals(ReportKind.RAW_FRAMING_RISK, ReportKind.fromRaw(12))
        assertEquals(ReportKind.NOTE, ReportKind.fromRaw(13))
    }

    @Test
    fun `ReportKind fromRaw degrades an unknown raw int rather than guessing a neighbour`() {
        // PD_REPORT_KIND_COUNT is 14: one past the end must not become NOTE by accident.
        assertEquals(ReportKind.UNRECOGNIZED, ReportKind.fromRaw(14))
        assertEquals(ReportKind.UNRECOGNIZED, ReportKind.fromRaw(-2))
    }

    @Test
    fun `RenderPath fromRaw matches pd_render_path`() {
        assertEquals(RenderPath.HARDWARE, RenderPath.fromRaw(0))
        assertEquals(RenderPath.RASTER, RenderPath.fromRaw(1))
        assertEquals(RenderPath.NOT_RENDERED, RenderPath.fromRaw(2))
        assertEquals(RenderPath.UNRECOGNIZED, RenderPath.fromRaw(3))
    }

    @Test
    fun `a report entry reads as one line with where, requested, delivered and why`() {
        val entry = ReportEntry(
            kind = ReportKind.UNSUPPORTED_BLOCK,
            block = "blocks[1]",
            requested = "barcode:code128 \"12345670\"",
            delivered = "omitted",
            path = RenderPath.NOT_RENDERED,
            note = "this profile has no barcode support"
        )
        // toString goes through abiName, which is a native call, so only the structure is
        // asserted here -- the six facts are all present and none is collapsed away.
        assertEquals("blocks[1]", entry.block)
        assertEquals("omitted", entry.delivered)
        assertTrue(entry.requested.contains("code128"))
        assertTrue(entry.note.isNotEmpty())
    }

    @Test
    fun `document meta carries no cut when the document asked for none`() {
        // What the JNI glue reports as hasCut == 0: the caller's JobOptions and then the
        // printer's profile decide, and nothing here invents a default (docs/receipt-dsl.md
        // "Cut control": JobOptions > document meta > printer profile).
        val silent = DocumentMeta(cut = null, topFeedDots = 0, bottomFeedDots = 0)
        assertNull(silent.cut)

        val declared = DocumentMeta(cut = Cut.FULL, topFeedDots = 24, bottomFeedDots = 160)
        assertEquals(Cut.FULL, declared.cut)
        assertEquals(24, declared.topFeedDots)
        assertEquals(160, declared.bottomFeedDots)
    }

    @Test
    fun `a rendered document compares by byte content rather than array identity`() {
        val meta = DocumentMeta(cut = null, topFeedDots = 0, bottomFeedDots = 0)
        val first = RenderedDocument(byteArrayOf(1, 2, 3), CodePage.PC437, meta, emptyList())
        val second = RenderedDocument(byteArrayOf(1, 2, 3), CodePage.PC437, meta, emptyList())
        assertEquals(first, second)
        assertEquals(first.hashCode(), second.hashCode())
    }
}
