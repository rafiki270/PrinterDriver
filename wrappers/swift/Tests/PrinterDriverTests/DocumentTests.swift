import CPrinterDriver
import CPrinterDriverTestSupport
import XCTest

@testable import PrinterDriver

/// M19 — the receipt DSL through the wrapper (docs/receipt-dsl.md).
///
/// End to end over the real engine, like every other suite here: the template is parsed,
/// bound and rendered by the C++ core, and the bytes are the bytes the scripted device
/// received. Nothing about the DSL is re-implemented in Swift, so what these tests prove
/// is that the Swift surface reaches the real one.
final class DocumentTests: XCTestCase {

  private func makeDriver() throws -> PrinterDriver {
    try PrinterDriver(fsyncDisabled: true)
  }

  /// A template with an `each` loop and the built-in `upper` formatter, plus a `meta` the
  /// job has to honour.
  private let orderTemplate = """
    { "v": 1, "template": true,
      "meta": { "cut": "full", "margins": { "topDots": 24 } },
      "blocks": [
        { "text": "{{venue.name|upper}}" },
        { "each": "order.items",
          "block": { "text": "{{qty}}x {{name|upper}}" } } ] }
    """

  private let orderModel = """
    { "venue": { "name": "my restaurant" },
      "order": { "items": [ { "qty": 2, "name": "pilsner" },
                            { "qty": 1, "name": "goulash" } ] } }
    """

  // MARK: - Printing

  func testPrintDocumentBindsATemplateAndPrintsItThroughTheOrdinaryEngine() throws {
    let driver = try makeDriver()
    let counter = try driver.scriptedPrinter(id: "counter", .healthy)

    let job = try counter.printDocument(
      orderTemplate, model: orderModel, options: JobOptions(key: "order-7F3A"))
    XCTAssertTrue(job.renderReport.isEmpty, "nothing should have degraded: \(job.renderReport)")
    XCTAssertEqual(job.key, "order-7F3A")

    runAsync(in: self) {
      switch await job.result {
      case .done(let confidence, let grade, _, let method):
        // A template job is an ordinary job: it earns exactly what the fence earns.
        XCTAssertEqual(confidence, .cutFaultFree)
        XCTAssertEqual(grade, .aJobLevelConfirmation)
        XCTAssertEqual(method, "GS(H) fn48")
      case .failed(let reason, _): XCTFail("expected done, got failed(\(reason))")
      case .unknown: XCTFail("expected done, got unknown")
      }
    }

    // The formatter ran, the loop repeated in model order, and no placeholder survived.
    XCTAssertTrue(counter.scriptedReceivedContains("MY RESTAURANT"))
    XCTAssertTrue(counter.scriptedReceivedContains("2x PILSNER"))
    XCTAssertTrue(counter.scriptedReceivedContains("1x GOULASH"))
    XCTAssertFalse(counter.scriptedReceivedContains("{{"))
    XCTAssertEqual(counter.scriptedCuts, 1)

    // Rule 2 of the idempotency contract reaches this entry point too.
    let again = try counter.printDocument(
      orderTemplate, model: orderModel, options: JobOptions(key: "order-7F3A"))
    XCTAssertTrue(again === job)
    XCTAssertEqual(counter.scriptedCuts, 1)
  }

  func testPrintDocumentAcceptsFoundationJSON() throws {
    let driver = try makeDriver()
    let counter = try driver.scriptedPrinter(id: "counter-json", .healthy)

    let document: [String: Any] = [
      "v": 1,
      "template": true,
      "blocks": [["text": "{{title|upper}}"]],
    ]
    let job = try counter.printDocument(document, model: ["title": "table 4"])
    XCTAssertTrue(job.renderReport.isEmpty)
    runAsync(in: self) { _ = await job.result }
    XCTAssertTrue(counter.scriptedReceivedContains("TABLE 4"))
  }

  // MARK: - Rendering, which prints nothing

  func testRenderDocumentReportsADeclaredDegradationAndPrintsNothing() throws {
    let driver = try makeDriver()
    let plain = try driver.scriptedPrinter(id: "no-gs-k", .noBarcode)

    let document = """
      { "v": 1, "blocks": [ { "text": "WIDGET CO" },
                            { "barcode": "12345670", "symbology": "code128" } ] }
      """
    let rendered = try plain.renderDocument(document)

    // The text still rendered: a declared degradation is not a failure.
    XCTAssertFalse(rendered.bytes.isEmpty)
    XCTAssertTrue(rendered.bytes.contains(Array("WIDGET CO".utf8)))

    XCTAssertEqual(rendered.report.count, 1)
    let entry = try XCTUnwrap(rendered.report.first)
    XCTAssertEqual(entry.kind, .unsupportedBlock)
    XCTAssertEqual(entry.kind.abiName, "unsupportedBlock")
    XCTAssertEqual(entry.block, "blocks[1]")
    XCTAssertTrue(entry.requested.contains("code128"))
    XCTAssertEqual(entry.delivered, "omitted")
    XCTAssertEqual(entry.path, .notRendered)
    XCTAssertFalse(entry.note.isEmpty)

    // Rendering is not printing.
    XCTAssertEqual(plain.scriptedPrintDataBytes, 0)
    XCTAssertEqual(plain.scriptedCuts, 0)
  }

  func testRenderDocumentReturnsTheDocumentsOwnMeta() throws {
    let driver = try makeDriver()
    let counter = try driver.scriptedPrinter(id: "meta", .healthy)

    let rendered = try counter.renderDocument(orderTemplate, model: orderModel)
    XCTAssertEqual(rendered.meta.cut, .full)
    XCTAssertEqual(rendered.meta.topFeedDots, 24)
    XCTAssertEqual(rendered.meta.bottomFeedDots, 0)
    XCTAssertTrue(rendered.report.isEmpty)
    XCTAssertTrue(rendered.bytes.contains(Array("MY RESTAURANT".utf8)))
  }

  func testRenderDocumentIsAvailableAsync() throws {
    let driver = try makeDriver()
    let counter = try driver.scriptedPrinter(id: "async", .healthy)
    runAsync(in: self) {
      let rendered = try await counter.renderDocument(
        self.orderTemplate, model: self.orderModel) as RenderedDocument
      XCTAssertFalse(rendered.bytes.isEmpty)
    }
  }

  // MARK: - Refusals

  func testMalformedDocumentsThrowAndPrintNothing() throws {
    let driver = try makeDriver()
    let counter = try driver.scriptedPrinter(id: "bad", .healthy)

    XCTAssertThrowsError(try counter.renderDocument("this is not json"))
    // A template with no model is refused rather than printed: a receipt full of
    // {{order.total}} is worse than no receipt, because it looks like one.
    XCTAssertThrowsError(try counter.renderDocument(orderTemplate))
    XCTAssertThrowsError(try counter.printDocument("this is not json"))

    XCTAssertEqual(counter.scriptedPrintDataBytes, 0)
    XCTAssertEqual(counter.scriptedCuts, 0)
  }

  // MARK: - M16, finally reachable

  /// docs/api.md §17.1: `registerFormatter` was accepted and stored, and nothing a
  /// wrapper could call consulted it. This is that call site.
  func testARegisteredFormatterFiresThroughThisPath() throws {
    let driver = try makeDriver()
    let template = """
      { "v": 1, "template": true, "blocks": [ { "text": "{{item|acme.stars}}" } ] }
      """
    let model = #"{"item":"tip"}"#

    // The control: no registration, so the name is unknown and the report says so.
    let control = try driver.scriptedPrinter(id: "fmt-control", .healthy)
    let before = try control.renderDocument(template, model: model)
    XCTAssertEqual(before.report.first?.kind, .unknownFormatter)
    XCTAssertFalse(before.bytes.contains(Array("***tip***".utf8)))

    try driver.register(
      formatter: TemplateFormatter(name: "acme.stars") { value, _, _ in "***\(value)***" })

    let counter = try driver.scriptedPrinter(id: "fmt", .healthy)
    let after = try counter.renderDocument(template, model: model)
    XCTAssertTrue(after.report.isEmpty, "expected no degradation, got \(after.report)")
    XCTAssertTrue(after.bytes.contains(Array("***tip***".utf8)))

    // And on paper, not only in a preview.
    let job = try counter.printDocument(template, model: model)
    runAsync(in: self) { _ = await job.result }
    XCTAssertTrue(counter.scriptedReceivedContains("***tip***"))
  }
}
