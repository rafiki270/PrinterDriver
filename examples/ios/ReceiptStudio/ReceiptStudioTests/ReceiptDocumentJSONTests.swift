//
//  ReceiptDocumentJSONTests.swift
//  ReceiptStudioTests
//
//  The designer's whole contract is that what it produces is a receipt-DSL
//  document — not something shaped like one. These tests assert against the
//  canonical JSON in docs/receipt-dsl.md, structurally rather than by string
//  comparison, so key order and whitespace stay free to change and the shape
//  does not.
//

import XCTest
@testable import ReceiptStudio

final class ReceiptDocumentJSONTests: XCTestCase {

    // MARK: Helpers

    private func object(_ document: ReceiptDocument) throws -> [String: Any] {
        let data = try document.jsonData(pretty: false)
        let decoded = try JSONSerialization.jsonObject(with: data)
        return try XCTUnwrap(decoded as? [String: Any])
    }

    private func blocks(_ document: ReceiptDocument) throws -> [[String: Any]] {
        let root = try object(document)
        return try XCTUnwrap(root["blocks"] as? [[String: Any]])
    }

    // MARK: Shape

    func testTopLevelShapeIsVersionStylesBlocks() throws {
        let root = try object(.sample)
        XCTAssertEqual(Set(root.keys), ["v", "styles", "blocks"])
        XCTAssertEqual(root["v"] as? Int, 1)
        XCTAssertNotNil(root["styles"] as? [String: Any])
        XCTAssertNotNil(root["blocks"] as? [Any])
    }

    func testNamedStyleSerialisesOnlyNonDefaultParameters() throws {
        let document = ReceiptDocument(
            styles: ["h1": TextStyle(bold: true, widthScale: 2, heightScale: 2, align: .center)],
            blocks: [ReceiptBlock(content: .text(content: "MY RESTAURANT", style: .named("h1")))])

        let root = try object(document)
        let styles = try XCTUnwrap(root["styles"] as? [String: Any])
        let h1 = try XCTUnwrap(styles["h1"] as? [String: Any])

        // Exactly the keys docs/receipt-dsl.md shows for this style: no underline,
        // no inverse, because the document never said anything about them.
        XCTAssertEqual(Set(h1.keys), ["bold", "widthScale", "heightScale", "align"])
        XCTAssertEqual(h1["bold"] as? Bool, true)
        XCTAssertEqual(h1["widthScale"] as? Int, 2)
        XCTAssertEqual(h1["heightScale"] as? Int, 2)
        XCTAssertEqual(h1["align"] as? String, "center")
    }

    func testTextBlockCarriesStyleByName() throws {
        let document = ReceiptDocument(
            styles: ["h1": TextStyle(bold: true)],
            blocks: [ReceiptBlock(content: .text(content: "MY RESTAURANT", style: .named("h1")))])

        let block = try XCTUnwrap(blocks(document).first)
        XCTAssertEqual(block["text"] as? String, "MY RESTAURANT")
        XCTAssertEqual(block["style"] as? String, "h1")
    }

    func testTextBlockCarriesInlineStyleAsAnObject() throws {
        let document = ReceiptDocument(blocks: [
            ReceiptBlock(content: .text(content: "Order 7F3A-92C1",
                                        style: .inline(TextStyle(align: .center)))),
        ])

        let block = try XCTUnwrap(blocks(document).first)
        let style = try XCTUnwrap(block["style"] as? [String: Any])
        XCTAssertEqual(style["align"] as? String, "center")
    }

    func testColumnsBlockMatchesTheDocumentedCellShape() throws {
        let document = ReceiptDocument(blocks: [
            ReceiptBlock(content: .columns(cells: [
                ColumnCell(content: "2× Pilsner", width: .flex),
                ColumnCell(content: "9.00", width: .chars(8), align: .right),
            ])),
        ])

        let block = try XCTUnwrap(blocks(document).first)
        let cells = try XCTUnwrap(block["columns"] as? [[String: Any]])
        XCTAssertEqual(cells.count, 2)

        XCTAssertEqual(cells[0]["content"] as? String, "2× Pilsner")
        XCTAssertEqual(cells[0]["width"] as? String, "flex")
        XCTAssertNil(cells[0]["align"])

        XCTAssertEqual(cells[1]["content"] as? String, "9.00")
        let width = try XCTUnwrap(cells[1]["width"] as? [String: Any])
        XCTAssertEqual(width["chars"] as? Int, 8)
        XCTAssertEqual(cells[1]["align"] as? String, "right")
    }

    func testDividerFeedAndQrBlocks() throws {
        let document = ReceiptDocument(blocks: [
            ReceiptBlock(content: .divider(.dashed)),
            ReceiptBlock(content: .feed(lines: 1)),
            ReceiptBlock(content: .qr(payload: "7F3A-92C1", size: 6, ec: .M, align: .center)),
        ])

        let encoded = try blocks(document)
        XCTAssertEqual(encoded[0]["divider"] as? String, "dashed")
        XCTAssertEqual(encoded[1]["feed"] as? Int, 1)
        XCTAssertEqual(encoded[2]["qr"] as? String, "7F3A-92C1")
        XCTAssertEqual(encoded[2]["size"] as? Int, 6)
        XCTAssertEqual(encoded[2]["ec"] as? String, "M")
        XCTAssertEqual(encoded[2]["align"] as? String, "center")
    }

    // MARK: Round trip

    func testDesignedDocumentRoundTripsThroughJson() throws {
        let original = ReceiptDocument.sample
        let restored = try ReceiptDocument.from(json: original.jsonData())
        XCTAssertEqual(restored, original)
    }

    func testRoundTripIsStableAcrossTwoEncodes() throws {
        let once = try ReceiptDocument.sample.jsonString()
        let twice = try ReceiptDocument.from(json: once).jsonString()
        XCTAssertEqual(once, twice)
    }

    /// The canonical example from docs/receipt-dsl.md, parsed by the app's own model.
    func testDecodesTheCanonicalExampleFromTheDesignDocument() throws {
        let json = """
        { "v": 1,
          "styles": { "h1": { "bold": true, "widthScale": 2, "heightScale": 2, "align": "center" } },
          "blocks": [
            { "text": "MY RESTAURANT", "style": "h1" },
            { "columns": [ { "content": "2× Pilsner", "width": "flex" },
                           { "content": "9.00", "width": { "chars": 8 }, "align": "right" } ] },
            { "qr": "7F3A-92C1", "size": 6, "ec": "M", "align": "center" } ] }
        """

        let document = try ReceiptDocument.from(json: json)
        XCTAssertEqual(document.version, 1)
        XCTAssertEqual(document.styles["h1"],
                       TextStyle(bold: true, widthScale: 2, heightScale: 2, align: .center))
        XCTAssertEqual(document.blocks.count, 3)

        guard case .text(let content, let style) = document.blocks[0].content else {
            return XCTFail("first block should be text")
        }
        XCTAssertEqual(content, "MY RESTAURANT")
        XCTAssertEqual(style, .named("h1"))

        guard case .columns(let cells) = document.blocks[1].content else {
            return XCTFail("second block should be columns")
        }
        XCTAssertEqual(cells.map(\.width), [.flex, .chars(8)])
        XCTAssertEqual(cells[1].align, .right)

        guard case .qr(let payload, let size, let ec, let align) = document.blocks[2].content else {
            return XCTFail("third block should be qr")
        }
        XCTAssertEqual(payload, "7F3A-92C1")
        XCTAssertEqual(size, 6)
        XCTAssertEqual(ec, .M)
        XCTAssertEqual(align, .center)

        // And it survives a trip back out through the app's encoder.
        XCTAssertEqual(try ReceiptDocument.from(json: document.jsonData()), document)
    }

    // MARK: Layout, which the preview and the ops share

    func testColumnRowFillsExactlyTheCharactersPerLine() {
        for width in PaperWidth.allCases {
            let line = ReceiptRenderer.layout(
                cells: [ColumnCell(content: "2× Pilsner", width: .flex),
                        ColumnCell(content: "9.00", width: .chars(8), align: .right)],
                columns: width.charactersPerLine)
            XCTAssertEqual(line.count, width.charactersPerLine, "at \(width.label)")
            XCTAssertTrue(line.hasPrefix("2× Pilsner"))
            XCTAssertTrue(line.hasSuffix("    9.00"))
        }
    }

    func testDoubleWidthHalvesTheColumns() {
        let style = TextStyle(widthScale: 2)
        XCTAssertEqual(ReceiptRenderer.columns(for: style, width: .mm80), 24)
        XCTAssertEqual(ReceiptRenderer.columns(for: .default, width: .mm80), 48)
        XCTAssertEqual(ReceiptRenderer.columns(for: .default, width: .mm58), 32)
    }

    func testOperationsFlattenEveryBlockKind() {
        let ops = ReceiptRenderer.operations(.sample, width: .mm80)
        XCTAssertFalse(ops.isEmpty)

        let feeds = ops.filter { if case .feed = $0.kind { return true } else { return false } }
        let codes = ops.filter { if case .qr = $0.kind { return true } else { return false } }
        XCTAssertEqual(feeds.count, 2)
        XCTAssertEqual(codes.count, 1)

        // Columns arrive pre-padded and left-aligned: the printer must not centre a
        // row that the layout already positioned.
        for op in ops {
            if case .text(let line) = op.kind, line.contains("TOTAL") {
                XCTAssertEqual(op.style.align, .left)
                XCTAssertEqual(line.count, PaperWidth.mm80.charactersPerLine)
            }
        }
    }

    func testWordWrapNeverExceedsTheLineWidth() {
        let text = "Thank you for visiting, please come again and bring the whole family"
        for columns in [16, 24, 32, 48] {
            for line in ReceiptRenderer.wrap(text, to: columns) {
                XCTAssertLessThanOrEqual(line.count, columns)
            }
        }
    }
}
