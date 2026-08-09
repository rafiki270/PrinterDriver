import CPrinterDriver
import Foundation

// M19 — the receipt DSL through the wrapper (docs/receipt-dsl.md, docs/api.md §3).
//
// The wrapper contains no rendering logic and no template engine. Which blocks a profile
// can draw, what a missing model path does, how `meta.cut` reaches a job — all of it is
// in the core behind `pd_render_document` and `pd_print_document_json`. What is here is
// the shape Swift callers expect: value types, a typed report instead of an opaque
// string, `async`, and `[String: Any]` overloads for the JSON everybody already has.

// MARK: - Enums

/// What kind of departure from the document was declared — `pd_report_kind`.
public enum ReportKind: UInt32, ABIMirroredEnum {
  /// `{{order.note}}` with no such path in the model.
  case missingPath = 0
  /// `{{v|frobnicate}}`.
  case unknownFormatter = 1
  /// `{{v|number:2}}` where `v` is an object.
  case unformattableValue = 2
  /// An unterminated `{{`, an empty path — and the three hard failures that stop bytes
  /// being produced at all: JSON that is not JSON, a structure that is not a document,
  /// and a template submitted with no model.
  case malformedTemplate = 3
  case unknownStyle = 4
  /// An `extends` chain that loops.
  case styleCycle = 5
  /// Italic, `rotate90`, a raster font on the hardware path.
  case unsupportedStyle = 6
  /// A symbology this build cannot draw, or a block this profile has no hardware for.
  case unsupportedBlock = 7
  case missingImage = 8
  /// Content clipped to fit the media width.
  case truncated = 9
  /// `each` over a missing or non-array path.
  case emptyIteration = 10
  /// An IANA zone name: this library ships no timezone database.
  case unsupportedTimezone = 11
  /// A `raw` block carrying a cut or a status command — the core owns job termination.
  case rawFramingRisk = 12
  /// Everything else worth telling the operator.
  case note = 13

  public static let abiTypeName = "ReportKind"
  /// Claims the least: an unrecognized kind is something worth saying and nothing more.
  public static let unrecognizedFallback = ReportKind.note

  /// The core's own spelling, from `pd_report_kind_name`.
  public var abiName: String {
    String(cString: pd_report_kind_name(pd_report_kind(rawValue)))
  }
}

/// How an entry's content reached the paper, if it did — `pd_render_path`.
public enum RenderPath: UInt32, ABIMirroredEnum {
  /// Produced by ESC/POS commands.
  case hardware = 0
  /// Produced as an image.
  case raster = 1
  /// Requested, and deliberately absent from the output.
  case notRendered = 2

  public static let abiTypeName = "RenderPath"
  /// Claims the least: if it is not known to have been drawn, it was not drawn.
  public static let unrecognizedFallback = RenderPath.notRendered

  /// The core's own spelling, from `pd_render_path_name`.
  public var abiName: String {
    String(cString: pd_render_path_name(pd_render_path(rawValue)))
  }
}

// MARK: - The render report

/// One declared degradation — `pd_report_entry`.
///
/// A missing model path, an unknown formatter and a barcode the profile cannot draw all
/// land here, and the receipt still prints. That is the honesty contract of
/// docs/receipt-dsl.md: a declared degradation is not a failure, but it is never silent.
public struct ReportEntry: Hashable, Sendable {
  public let kind: ReportKind
  /// Where it happened, as a path into the document — `"blocks[3].cells[1]"`,
  /// `"meta.tz"`, or `"document"` for one that belongs to no block.
  public let block: String
  /// What the document asked for, in the words it is printed in.
  public let requested: String
  /// What the paper got. Often `"omitted"`.
  public let delivered: String
  public let path: RenderPath
  /// Why, when the pair above does not say it all. Empty when it does.
  public let note: String

  init(_ wire: pd_report_entry) {
    kind = ReportKind(bridging: wire.kind.rawValue)
    block = String(cString: wire.block)
    requested = String(cString: wire.requested)
    delivered = String(cString: wire.delivered)
    path = RenderPath(bridging: wire.path.rawValue)
    note = String(cString: wire.note)
  }

  /// One line, for a log or a test failure message.
  public var description: String {
    let reason = note.isEmpty ? "" : " - \(note)"
    return "\(block)  \(kind.abiName): requested \"\(requested)\", delivered "
      + "\"\(delivered)\" [\(path.abiName)]\(reason)"
  }
}

/// What a document asks the engine for, read back from its `meta` — the cut and the
/// margins of docs/receipt-dsl.md.
///
/// Reported here and applied by ``Printer/printDocument(_:model:options:)`` under that
/// document's precedence: what the caller put in ``JobOptions`` wins, this fills in what
/// the caller left alone, and the printer's profile answers what neither said.
public struct DocumentMeta: Hashable, Sendable {
  /// `nil` when the document asked for no particular cut.
  public let cut: Cut?
  /// Blank paper before the first content line. 0 when the document said nothing.
  public let topFeedDots: UInt32
  /// The *total* whitespace between the last content and the cut. The engine feeds
  /// `max(the profile's blade clearance, this)`, so no document can clip its own trailer.
  public let bottomFeedDots: UInt32
}

/// A rendered document: the bytes a printer would receive, and everything that was
/// declared along the way.
public struct RenderedDocument: Hashable, Sendable {
  /// The ESC/POS the renderer produced. Never the whole job: the engine adds its own
  /// initialise, trailing feed, cut and completion fence around this.
  public let bytes: Data
  public let codePage: CodePage
  public let meta: DocumentMeta
  /// Empty when every block rendered exactly as written.
  public let report: [ReportEntry]
}

// MARK: - Options

/// Options for ``Printer/renderDocument(_:model:options:)``. The useful call is
/// `renderDocument(json)`.
public struct RenderOptions: Hashable, Sendable {
  /// 0 lays the document out for this printer's own configured width, which is what the
  /// engine rasterizes to. Anything else previews a different media width.
  public var widthDots: UInt32
  /// Extra whitespace before a **mid-document** `cut` block. The renderer feeds
  /// `max(the profile's blade clearance, this)`: more is always granted, less never.
  public var cutClearanceDots: UInt16
  /// 0 means 1024 — the Epson tall-image split, applied to `image` blocks.
  public var maxRowsPerBand: UInt32
  /// Overrides the document's own `meta.locale`. `nil` defers to it.
  public var locale: String?
  /// Overrides `meta.currency`.
  public var currency: String?
  /// Overrides `meta.tz`. A fixed offset such as `"+02:00"`; an IANA name is reported as
  /// an ``ReportKind/unsupportedTimezone`` degradation, because there is no tz database
  /// in a core that ships no dependencies.
  public var timeZone: String?

  public init(
    widthDots: UInt32 = 0,
    cutClearanceDots: UInt16 = 0,
    maxRowsPerBand: UInt32 = 0,
    locale: String? = nil,
    currency: String? = nil,
    timeZone: String? = nil
  ) {
    self.widthDots = widthDots
    self.cutClearanceDots = cutClearanceDots
    self.maxRowsPerBand = maxRowsPerBand
    self.locale = locale
    self.currency = currency
    self.timeZone = timeZone
  }
}

// MARK: - Printer

extension Printer {
  /// Renders a receipt-DSL document against this printer's capability profile.
  ///
  /// **Nothing prints and no job exists.** This is the preview path: the bytes a printer
  /// would receive, plus every degradation the profile forced, before any paper is
  /// committed.
  ///
  /// - Parameters:
  ///   - documentJSON: a document or a template (docs/receipt-dsl.md).
  ///   - model: the parameter model bound into a template — `{{path.to.value}}`, `each`,
  ///     `if`. `nil` for a document that carries its own content; a template handed no
  ///     model is refused rather than printed, because a receipt full of
  ///     `{{order.total}}` is worse than no receipt.
  /// - Throws: ``PrinterDriverError`` when the JSON is not JSON, the structure is not a
  ///   document, or a template arrived without a model. Everything softer is a
  ///   ``ReportEntry`` and still renders.
  public func renderDocument(
    _ documentJSON: String,
    model: String? = nil,
    options: RenderOptions = RenderOptions()
  ) throws -> RenderedDocument {
    var wire = pd_render_options()
    wire.width_dots = options.widthDots
    wire.cut_clearance_dots = options.cutClearanceDots
    wire.max_rows_per_band = options.maxRowsPerBand

    var out = pd_render_result()
    let ok = withOptionalCString(options.locale) { locale -> Int32 in
      wire.locale = locale
      return withOptionalCString(options.currency) { currency -> Int32 in
        wire.currency = currency
        return withOptionalCString(options.timeZone) { zone -> Int32 in
          wire.tz = zone
          return documentJSON.withCString { document -> Int32 in
            withOptionalCString(model) { modelPointer -> Int32 in
              withUnsafePointer(to: &wire) { optionsPointer in
                pd_render_document(
                  core.handle, handle, document, modelPointer, optionsPointer, &out)
              }
            }
          }
        }
      }
    }
    guard ok != 0 else {
      // The report is read before the error is thrown: a caller that wants to show what
      // went wrong gets the same list it would show for a degradation.
      throw PrinterDriverError(
        "\(core.lastError().message)\n\(core.renderReport().map(\.description).joined(separator: "\n"))"
      )
    }
    return RenderedDocument(
      bytes: out.bytes.map { Data(bytes: $0, count: out.size) } ?? Data(),
      codePage: CodePage(bridging: out.code_page.rawValue),
      meta: DocumentMeta(out),
      report: core.renderReport())
  }

  /// Renders a document supplied as Foundation JSON.
  ///
  /// The dictionary is serialized here and parsed by the core's own strict RFC 8259
  /// reader, so what is rendered is exactly what a stored `.json` template would render.
  public func renderDocument(
    _ document: [String: Any],
    model: [String: Any]? = nil,
    options: RenderOptions = RenderOptions()
  ) throws -> RenderedDocument {
    try renderDocument(
      Printer.jsonText(document, what: "document"),
      model: try model.map { try Printer.jsonText($0, what: "model") },
      options: options)
  }

  /// Renders `documentJSON` off the calling thread.
  public func renderDocument(
    _ documentJSON: String,
    model: String? = nil,
    options: RenderOptions = RenderOptions()
  ) async throws -> RenderedDocument {
    let printer = self
    return try await withCheckedThrowingContinuation { continuation in
      DispatchQueue.global(qos: .userInitiated).async {
        do {
          continuation.resume(
            returning: try printer.renderDocument(documentJSON, model: model, options: options)
              as RenderedDocument)
        } catch {
          continuation.resume(throwing: error)
        }
      }
    }
  }

  /// Renders a receipt-DSL document and prints it.
  ///
  /// The job is an ordinary ``PrintJob``: same worker, same preflight, same completion
  /// fence, same confidence grading, same idempotency-key dedupe as
  /// ``print(_:options:)``. There is no second engine — a template job proves exactly
  /// what a raster job proves.
  ///
  /// The document's `meta` reaches the job through `options`: an all-defaults
  /// ``JobOptions`` lets the document decide its own cut and margins, and any field set
  /// explicitly wins over it.
  ///
  /// - Returns: the ordinary ``PrintJob``, carrying what the renderer declared in
  ///   ``PrintJob/renderReport``. That is worth reading on the success path: a receipt
  ///   that printed with a dropped barcode is a receipt that printed.
  /// - Throws: ``PrinterDriverError`` when there are no bytes to send. Nothing is
  ///   submitted in that case — a malformed document never becomes a blank receipt.
  @discardableResult
  public func printDocument(
    _ documentJSON: String,
    model: String? = nil,
    options: JobOptions = JobOptions()
  ) throws -> PrintJob {
    let jobHandle = try documentJSON.withCString { document -> OpaquePointer in
      try withOptionalCString(model) { modelPointer -> OpaquePointer in
        try options.withABI { optionsPointer -> OpaquePointer in
          guard
            let job = pd_print_document_json(
              core.handle, handle, document, modelPointer, optionsPointer, nil)
          else {
            throw PrinterDriverError(
              "\(core.lastError().message)\n\(core.renderReport().map(\.description).joined(separator: "\n"))"
            )
          }
          return job
        }
      }
    }
    let job = core.internJob(jobHandle, mechanism: completionMechanism)
    job.setRenderReport(core.renderReport())
    return job
  }

  /// Prints a document supplied as Foundation JSON.
  @discardableResult
  public func printDocument(
    _ document: [String: Any],
    model: [String: Any]? = nil,
    options: JobOptions = JobOptions()
  ) throws -> PrintJob {
    try printDocument(
      Printer.jsonText(document, what: "document"),
      model: try model.map { try Printer.jsonText($0, what: "model") },
      options: options)
  }

  private static func jsonText(_ value: [String: Any], what: String) throws -> String {
    guard JSONSerialization.isValidJSONObject(value) else {
      throw PrinterDriverError("the \(what) dictionary is not representable as JSON")
    }
    let data = try JSONSerialization.data(withJSONObject: value, options: [])
    guard let text = String(data: data, encoding: .utf8) else {
      throw PrinterDriverError("the \(what) dictionary did not serialize to UTF-8")
    }
    return text
  }
}

extension DocumentMeta {
  init(_ wire: pd_render_result) {
    cut = wire.has_cut != 0 ? Cut(bridging: wire.cut.rawValue) : nil
    topFeedDots = wire.top_feed_dots
    bottomFeedDots = wire.bottom_feed_dots
  }
}

extension DriverCore {
  /// The last render's report, pulled out through the ABI's indexed reader.
  ///
  /// Read immediately after the call that produced it: pd.h owns these strings until the
  /// next render on the same driver, and this is where they stop being borrowed.
  func renderReport() -> [ReportEntry] {
    let count = Int(pd_render_report_count(handle))
    var entries: [ReportEntry] = []
    entries.reserveCapacity(count)
    for index in 0..<count {
      var wire = pd_report_entry()
      if pd_render_report_at(handle, Int32(index), &wire) == 1 {
        entries.append(ReportEntry(wire))
      }
    }
    return entries
  }
}
