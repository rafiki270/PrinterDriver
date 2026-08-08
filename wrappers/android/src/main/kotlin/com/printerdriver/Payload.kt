package com.printerdriver

import android.graphics.Bitmap
import java.nio.ByteBuffer

/**
 * The three payload tiers (docs/api.md §3). All three produce identical feedback --
 * pick per call, mix freely. This sealed class IS the wire discriminator: unlike
 * pd_payload's C union (kind enum + tagged union), there is no separate "kind" value
 * to keep in sync here -- [Printer.print] dispatches on the subtype directly to one of
 * three native entry points (printRaster/printDocument/printRaw), each of which
 * already knows which pd_payload_kind it is building on the native side.
 */
sealed class Payload {

    /**
     * Tier 1 -- Raster, the web-POS drop-in path. The core composites alpha over white
     * paper, converts to grey with ITU-R 601 luma weights, scales to the printer's dot
     * width, binarizes and bands (the 1024px Epson tall-image split) -- all in integer
     * arithmetic, so the same pixels always produce the same bytes.
     *
     * [pixels] must be tightly-packed or [strideBytes]-strided RGBA8, top-to-bottom,
     * left-to-right, 4 bytes/pixel in R,G,B,A order. Prefer [of] over the raw
     * constructor unless you already have RGBA8 bytes from a non-Bitmap source (e.g.
     * relayed from the C ABI test harness, or decoded PNG bytes).
     */
    class Raster private constructor(
        val pixels: ByteArray,
        val width: Int,
        val height: Int,
        /** 0 -> tightly packed (pd.h: `stride_bytes` 0 means width * 4). */
        val strideBytes: Int,
        val binarization: Binarization,
        /** Only read for [Binarization.FIXED_THRESHOLD]; 0 -> the core's default (128). */
        val threshold: Int,
        /** 0 -> the core's default (1024, the Epson tall-image split). */
        val maxRowsPerBand: Int
    ) : Payload() {
        companion object {
            /**
             * The drop-in path api.md §3 describes ("platform bitmap: ... Android
             * Bitmap"). Requires [Bitmap.Config.ARGB_8888]: that is the one config
             * Android guarantees as 4 bytes/pixel, and -- because every supported ABI
             * here is little-endian -- [Bitmap.copyPixelsToBuffer] reads those 4 bytes
             * back out in R,G,B,A order despite the config's name, which happens to be
             * exactly the byte order pd_raster_rgba8 documents. This is a real,
             * well-known Android gotcha, not an assumption: the "ARGB" in the config
             * name describes the 32-bit-int channel order (0xAARRGGBB), not the
             * in-memory byte order once that int is read one byte at a time.
             */
            fun of(
                bitmap: Bitmap,
                binarization: Binarization = Binarization.FLOYD_STEINBERG,
                threshold: Int = 0,
                maxRowsPerBand: Int = 0
            ): Raster {
                require(bitmap.config == Bitmap.Config.ARGB_8888) {
                    "Payload.Raster.of requires Bitmap.Config.ARGB_8888 (got " +
                        "${bitmap.config}); convert first, e.g. " +
                        "bitmap.copy(Bitmap.Config.ARGB_8888, false)"
                }
                val buffer = ByteBuffer.allocate(bitmap.byteCount)
                bitmap.copyPixelsToBuffer(buffer)
                return Raster(
                    pixels = buffer.array(),
                    width = bitmap.width,
                    height = bitmap.height,
                    strideBytes = 0,
                    binarization = binarization,
                    threshold = threshold,
                    maxRowsPerBand = maxRowsPerBand
                )
            }

            /** Escape hatch for callers that already have raw RGBA8 bytes. */
            fun ofBytes(
                pixels: ByteArray,
                width: Int,
                height: Int,
                strideBytes: Int = 0,
                binarization: Binarization = Binarization.FLOYD_STEINBERG,
                threshold: Int = 0,
                maxRowsPerBand: Int = 0
            ): Raster = Raster(pixels, width, height, strideBytes, binarization, threshold, maxRowsPerBand)
        }
    }

    /**
     * Tier 2 -- document builder, for apps without a renderer. Standard receipt
     * semantics only (docs/api.md §3): text, style, alignment, feed. Encodes to the
     * conservative ESC/POS subset; [codePage] picks how non-ASCII text is
     * transliterated (lossily -- pd.h: `text` is "UTF-8; transliterated to the
     * document's code page, lossily").
     */
    data class Document(
        val ops: List<DocumentOp>,
        val codePage: CodePage = CodePage.PC437
    ) : Payload()

    /**
     * Tier 3 -- raw bytes, the escape hatch. The core still wraps it: preflight
     * before, completion fence + cut + fence after (docs/api.md §3) -- so even raw
     * jobs get real completion feedback. Must not embed its own cuts or realtime
     * status tricks; the core's trailing fence assumes it owns job termination.
     */
    data class Raw(val bytes: ByteArray) : Payload()
}

/**
 * One operation in a [Payload.Document]. Mirrors pd_op_kind's five members
 * (docs/api.md §3: "everything a receipt needs and nothing a layout engine would
 * need") as a sealed class instead of the C struct's (kind, text, value) triple --
 * still a mechanical, zero-decision translation (flattened back into pd_op's shape by
 * [Printer.print] before crossing into JNI), just one that lets the compiler catch a
 * missing case instead of an unchecked `value` field.
 */
sealed class DocumentOp {
    /** `text`, no line break. */
    data class Text(val text: String) : DocumentOp()

    /** `text` followed by LF; `null` emits a bare LF. */
    data class Line(val text: String? = null) : DocumentOp()

    data class Align(val alignment: Alignment) : DocumentOp()

    data class Bold(val enabled: Boolean) : DocumentOp()

    /** Line count, 1..255 (pd.h: PD_OP_FEED's `value`). */
    data class Feed(val lines: Int) : DocumentOp()
}
