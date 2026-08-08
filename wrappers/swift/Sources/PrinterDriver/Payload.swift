import CPrinterDriver
import Foundation

#if canImport(CoreGraphics)
  import CoreGraphics
#endif

/// One document-tier instruction — a mirror of `pd_op`.
///
/// Deliberately minimal: everything a receipt needs and nothing a layout engine would
/// need. Anything richer belongs in the raster tier, where the app owns the renderer.
public enum DocumentOp: Hashable, Sendable {
  /// Text with no line break.
  case text(String)
  /// Text followed by LF. `nil` emits a bare LF.
  case line(String?)
  case align(Alignment)
  case bold(Bool)
  /// Feed `lines` blank lines. The ABI accepts 1...255; 0 is rejected by the core.
  case feed(lines: UInt8)
}

/// The raster tier's input — a mirror of `pd_raster_rgba8`.
///
/// Straight (non-premultiplied) RGBA8, exactly as a platform bitmap hands it over. The
/// core composites alpha over white paper, converts to grey with the ITU-R 601 luma
/// weights, scales to the printer's dot width, binarizes and bands, all in integer
/// arithmetic — so the same pixels always produce the same bytes. None of that happens
/// here.
public struct Raster: Hashable, Sendable {
  /// Row-major RGBA8. At least `height * strideBytes` bytes (or `height * width * 4`
  /// when `strideBytes` is 0).
  public var rgba: Data
  public var width: UInt32
  public var height: UInt32
  /// Bytes per row. 0 means tightly packed.
  public var strideBytes: UInt32
  public var binarization: Binarization
  /// Read only for ``Binarization/fixedThreshold``. 0 means 128.
  public var threshold: UInt8
  /// 0 means 1024 — the Epson tall-image split.
  public var maxRowsPerBand: UInt32

  public init(
    rgba: Data,
    width: UInt32,
    height: UInt32,
    strideBytes: UInt32 = 0,
    binarization: Binarization = .floydSteinberg,
    threshold: UInt8 = 0,
    maxRowsPerBand: UInt32 = 0
  ) {
    self.rgba = rgba
    self.width = width
    self.height = height
    self.strideBytes = strideBytes
    self.binarization = binarization
    self.threshold = threshold
    self.maxRowsPerBand = maxRowsPerBand
  }
}

/// What to print — the three tiers of docs/api.md §3.
///
/// All three produce identical feedback, because the core appends the same completion
/// fences regardless of what the payload was. Pick per call, mix freely.
public enum Payload: Sendable {
  /// Tier 1. A finished bitmap; the app keeps its own renderer.
  case raster(Raster)

  /// Tier 2. Receipt semantics for apps without a renderer.
  case document(ops: [DocumentOp], codePage: CodePage)

  /// Tier 3. Passed through verbatim.
  ///
  /// - Important: Raw payloads must not embed their own cuts or realtime status tricks.
  ///   The core owns job termination and its trailing fence assumes that.
  case raw(Data)

  // MARK: Convenience constructors

  /// Tier 2, the common case: a run of lines.
  public static func text(_ lines: [String], codePage: CodePage = .pc437) -> Payload {
    .document(ops: lines.map { DocumentOp.line($0) }, codePage: codePage)
  }

  /// Tier 1 from 8-bit grey, one byte per pixel, row-major and tightly packed.
  ///
  /// The bytes are widened to opaque RGBA for the ABI. That round-trip is exact, not
  /// approximate: the core's luma weights sum to 256, so grey `v` comes back as `v`.
  public static func raster(
    grayscale: Data,
    width: UInt32,
    height: UInt32,
    binarization: Binarization = .floydSteinberg,
    threshold: UInt8 = 0,
    maxRowsPerBand: UInt32 = 0
  ) throws -> Payload {
    let expected = Int(width) * Int(height)
    guard width > 0, height > 0, grayscale.count >= expected else {
      throw PrinterDriverError(
        "grayscale raster needs \(expected) bytes for \(width)x\(height), got \(grayscale.count)"
      )
    }
    var rgba = Data(count: expected * 4)
    rgba.withUnsafeMutableBytes { destination in
      grayscale.withUnsafeBytes { source in
        let out = destination.bindMemory(to: UInt8.self)
        let input = source.bindMemory(to: UInt8.self)
        for index in 0..<expected {
          let value = input[index]
          out[index * 4 + 0] = value
          out[index * 4 + 1] = value
          out[index * 4 + 2] = value
          out[index * 4 + 3] = 255
        }
      }
    }
    return .raster(
      Raster(
        rgba: rgba, width: width, height: height, binarization: binarization,
        threshold: threshold, maxRowsPerBand: maxRowsPerBand))
  }

  #if canImport(CoreGraphics)
    /// Tier 1 from a platform bitmap — the drop-in path for an app that already renders
    /// its receipts.
    ///
    /// The image is drawn onto opaque white at its own pixel size before it reaches the
    /// ABI, because paper is white and the app's compositing intent should not be
    /// re-interpreted downstream. Everything after that — scaling to the printer's dot
    /// width, dithering, banding, pacing — happens in the core.
    public static func raster(
      _ image: CGImage,
      binarization: Binarization = .floydSteinberg,
      threshold: UInt8 = 0,
      maxRowsPerBand: UInt32 = 0
    ) throws -> Payload {
      let width = image.width
      let height = image.height
      guard width > 0, height > 0 else {
        throw PrinterDriverError("raster payload needs a non-zero image size")
      }
      let bytesPerRow = width * 4
      var rgba = Data(count: bytesPerRow * height)
      let drew = rgba.withUnsafeMutableBytes { buffer -> Bool in
        guard
          let base = buffer.baseAddress,
          let context = CGContext(
            data: base,
            width: width,
            height: height,
            bitsPerComponent: 8,
            bytesPerRow: bytesPerRow,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else {
          return false
        }
        let frame = CGRect(x: 0, y: 0, width: CGFloat(width), height: CGFloat(height))
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(frame)
        context.draw(image, in: frame)
        return true
      }
      guard drew else {
        throw PrinterDriverError("could not open a bitmap context for the image")
      }
      return .raster(
        Raster(
          rgba: rgba, width: UInt32(width), height: UInt32(height),
          strideBytes: UInt32(bytesPerRow), binarization: binarization,
          threshold: threshold, maxRowsPerBand: maxRowsPerBand))
    }
  #endif

  /// The tier this payload will be submitted as.
  public var kind: PayloadKind {
    switch self {
    case .raster: return .raster
    case .document: return .document
    case .raw: return .raw
    }
  }

  // MARK: ABI lowering

  /// Runs `body` with a C view of this payload.
  ///
  /// Every buffer handed over stays alive for the duration of the call and no longer,
  /// which is all `pd_print` needs: it copies pixels, ops and bytes before returning.
  func withABI<R>(_ body: (UnsafePointer<pd_payload>) throws -> R) rethrows -> R {
    switch self {
    case .raster(let raster):
      return try raster.rgba.withUnsafeBytes { bytes in
        var payload = pd_payload()
        payload.kind = PD_PAYLOAD_RASTER_RGBA8
        payload.as.raster = pd_raster_rgba8(
          pixels: bytes.bindMemory(to: UInt8.self).baseAddress,
          width: raster.width,
          height: raster.height,
          stride_bytes: raster.strideBytes,
          binarization: pd_binarization(raster.binarization.rawValue),
          threshold: raster.threshold,
          max_rows_per_band: raster.maxRowsPerBand)
        return try withUnsafePointer(to: &payload) { try body($0) }
      }

    case .document(let ops, let codePage):
      return try Payload.withDocumentOps(ops) { pointer, count in
        var payload = pd_payload()
        payload.kind = PD_PAYLOAD_DOCUMENT
        payload.as.document = pd_document(
          ops: pointer, count: count, code_page: pd_code_page(codePage.rawValue))
        return try withUnsafePointer(to: &payload) { try body($0) }
      }

    case .raw(let data):
      return try data.withUnsafeBytes { bytes in
        var payload = pd_payload()
        payload.kind = PD_PAYLOAD_RAW
        payload.as.raw = pd_raw(
          bytes: bytes.bindMemory(to: UInt8.self).baseAddress, size: data.count)
        return try withUnsafePointer(to: &payload) { try body($0) }
      }
    }
  }

  /// Flattens the ops' UTF-8 into one scratch buffer and hands out interior pointers.
  ///
  /// `pd_op.text` is a borrowed `const char*`; the ABI copies it during the call, so a
  /// single allocation released on the way out is enough, and it keeps the pointers
  /// stable in a way that nesting `withCString` per op could not.
  private static func withDocumentOps<R>(
    _ ops: [DocumentOp],
    _ body: (UnsafePointer<pd_op>?, Int) throws -> R
  ) rethrows -> R {
    var scratch: [CChar] = []
    var offsets: [Int?] = []
    offsets.reserveCapacity(ops.count)
    for op in ops {
      switch op {
      case .text(let value), .line(.some(let value)):
        offsets.append(scratch.count)
        scratch.append(contentsOf: value.utf8CString)
      default:
        offsets.append(nil)
      }
    }

    let storage = UnsafeMutableBufferPointer<CChar>.allocate(capacity: max(scratch.count, 1))
    defer { storage.deallocate() }
    _ = storage.initialize(fromContentsOf: scratch)

    var lowered: [pd_op] = []
    lowered.reserveCapacity(ops.count)
    for (index, op) in ops.enumerated() {
      let text: UnsafePointer<CChar>? = offsets[index].flatMap { offset in
        storage.baseAddress.map { UnsafePointer($0 + offset) }
      }
      switch op {
      case .text:
        lowered.append(pd_op(kind: PD_OP_TEXT, text: text, value: 0))
      case .line:
        lowered.append(pd_op(kind: PD_OP_LINE, text: text, value: 0))
      case .align(let alignment):
        lowered.append(pd_op(kind: PD_OP_ALIGN, text: nil, value: Int32(alignment.rawValue)))
      case .bold(let enabled):
        lowered.append(pd_op(kind: PD_OP_BOLD, text: nil, value: enabled ? 1 : 0))
      case .feed(let lines):
        lowered.append(pd_op(kind: PD_OP_FEED, text: nil, value: Int32(lines)))
      }
    }

    return try lowered.withUnsafeBufferPointer { try body($0.baseAddress, lowered.count) }
  }
}
