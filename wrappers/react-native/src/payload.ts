/**
 * The three payload tiers (docs/api.md §3), as constructors an app can read.
 *
 *   Payloads.text([...])                 tier 2, the one-liner
 *   Payloads.document(new Receipt()...)  tier 2, styled
 *   Payloads.raster({pixels, width, …})  tier 1, RGBA8 straight from a canvas
 *   Payloads.raw(bytes)                  tier 3, verbatim
 *
 * All three produce identical feedback, because the core appends the completion fences
 * regardless of what the payload was.
 */

import type {
  Alignment,
  Binarization,
  CodePage,
} from './enums.ts';
import type {
  DocumentOp,
  DocumentPayload,
  Payload,
  RasterPayload,
  RawPayload,
} from './types.ts';

/**
 * The tier-2 builder. Deliberately minimal — everything a receipt needs and nothing a
 * layout engine would need. Richer documents belong in the raster tier.
 */
export class Receipt {
  private readonly ops: DocumentOp[] = [];
  private readonly page: CodePage | undefined;

  constructor(codePage?: CodePage) {
    this.page = codePage;
  }

  /** Text with no line break. */
  text(text: string): this {
    this.ops.push({ kind: 'text', text });
    return this;
  }

  /** Text followed by LF. With no argument, a bare LF. */
  line(text?: string): this {
    this.ops.push(text === undefined ? { kind: 'line' } : { kind: 'line', text });
    return this;
  }

  align(value: Alignment): this {
    this.ops.push({ kind: 'align', value });
    return this;
  }

  bold(value = true): this {
    this.ops.push({ kind: 'bold', value });
    return this;
  }

  /** Feed `lines` lines. 1..255. */
  feed(lines = 1): this {
    this.ops.push({ kind: 'feed', lines });
    return this;
  }

  build(): DocumentPayload {
    return { kind: 'document', ops: [...this.ops], codePage: this.page };
  }
}

/**
 * A whole `ArrayBuffer` crosses to the native side without a copy. A typed-array VIEW is
 * copied first, because a view may be a window onto a much larger buffer — Node's `Buffer`
 * is famously a slice of a shared pool — and handing the whole pool to the printer would
 * be both wrong and a leak of unrelated memory.
 */
export function toArrayBuffer(bytes: ArrayBuffer | ArrayBufferView): ArrayBuffer {
  if (ArrayBuffer.isView(bytes)) {
    const view = new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const copy = new ArrayBuffer(view.byteLength);
    new Uint8Array(copy).set(view);
    return copy;
  }
  return bytes;
}

export const Payloads = {
  /** Tier 2, the common case: one line each, left aligned, profile code page. */
  text(lines: readonly string[], codePage?: CodePage): DocumentPayload {
    const receipt = new Receipt(codePage);
    for (const line of lines) receipt.line(line);
    return receipt.build();
  },

  /** Tier 2, styled. Accepts a `Receipt` or a bare op list. */
  document(source: Receipt | readonly DocumentOp[], codePage?: CodePage): DocumentPayload {
    if (source instanceof Receipt) return source.build();
    return { kind: 'document', ops: [...source], codePage };
  },

  /**
   * Tier 1. `pixels` is RGBA8 exactly as a canvas, `UIImage`/`CGImage` or Android
   * `Bitmap` hands it over. An `ArrayBuffer` crosses to the native side without a copy;
   * a typed-array view is copied first, because only a whole buffer can be shared.
   */
  raster(image: {
    pixels: ArrayBuffer | ArrayBufferView;
    width: number;
    height: number;
    strideBytes?: number;
    binarization?: Binarization;
    threshold?: number;
    maxRowsPerBand?: number;
  }): RasterPayload {
    return {
      kind: 'raster',
      pixels: toArrayBuffer(image.pixels),
      width: image.width,
      height: image.height,
      strideBytes: image.strideBytes,
      binarization: image.binarization,
      threshold: image.threshold,
      maxRowsPerBand: image.maxRowsPerBand,
    };
  },

  /**
   * Tier 3. Passed through verbatim. Must not embed its own cuts or realtime status
   * tricks: the core owns job termination, and its trailing fence assumes that.
   */
  raw(bytes: ArrayBuffer | ArrayBufferView): RawPayload {
    return { kind: 'raw', bytes: toArrayBuffer(bytes) };
  },
};

export type { Payload };
