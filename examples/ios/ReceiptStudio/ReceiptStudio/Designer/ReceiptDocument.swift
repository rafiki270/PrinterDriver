//
//  ReceiptDocument.swift
//  ReceiptStudio
//
//  The receipt DSL document model, exactly as docs/receipt-dsl.md defines it:
//  a plain data model with named styles and a vertical list of blocks. Design
//  once, store, send over the wire, render anywhere.
//
//  The canonical form is JSON:
//
//    { "v": 1,
//      "styles": { "h1": { "bold": true, "widthScale": 2, "align": "center" } },
//      "blocks": [
//        { "text": "MY RESTAURANT", "style": "h1" },
//        { "columns": [ { "content": "2× Pilsner", "width": "flex" },
//                       { "content": "9.00", "width": { "chars": 8 }, "align": "right" } ] },
//        { "divider": "dashed" },
//        { "feed": 1 },
//        { "qr": "7F3A-92C1", "size": 6, "ec": "M", "align": "center" } ] }
//
//  Blocks are discriminated by which key is present, so each case codes itself.
//  Fields at their default value are omitted: a document that says nothing about
//  underline is not the same document as one that says underline is off, and the
//  round-trip has to preserve that distinction to stay a faithful DSL document.
//

import Foundation

// MARK: - Style

enum TextAlign: String, Codable, CaseIterable, Sendable {
    case left, center, right

    var label: String {
        switch self {
        case .left: return "Left"
        case .center: return "Center"
        case .right: return "Right"
        }
    }
}

enum UnderlineStyle: String, Codable, CaseIterable, Sendable {
    case none, single, double

    /// ESC - n operand.
    var dots: UInt8 {
        switch self {
        case .none: return 0
        case .single: return 1
        case .double: return 2
        }
    }

    var label: String {
        switch self {
        case .none: return "None"
        case .single: return "Single"
        case .double: return "Double"
        }
    }
}

/// docs/receipt-dsl.md "TextStyle — the full parameter set", narrowed to the
/// parameters this example's editor exposes. Anything not listed here is simply
/// absent from the document rather than defaulted into it.
struct TextStyle: Codable, Equatable, Hashable, Sendable {
    var bold: Bool = false
    var underline: UnderlineStyle = .none
    var inverse: Bool = false
    var widthScale: Int = 1
    var heightScale: Int = 1
    var align: TextAlign = .left

    static let `default` = TextStyle()

    var isDefault: Bool { self == TextStyle.default }

    private enum CodingKeys: String, CodingKey {
        case bold, underline, inverse, widthScale, heightScale, align
    }

    init() {}

    init(bold: Bool = false,
         underline: UnderlineStyle = .none,
         inverse: Bool = false,
         widthScale: Int = 1,
         heightScale: Int = 1,
         align: TextAlign = .left) {
        self.bold = bold
        self.underline = underline
        self.inverse = inverse
        self.widthScale = widthScale
        self.heightScale = heightScale
        self.align = align
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        bold = try container.decodeIfPresent(Bool.self, forKey: .bold) ?? false
        underline = try container.decodeIfPresent(UnderlineStyle.self, forKey: .underline) ?? .none
        inverse = try container.decodeIfPresent(Bool.self, forKey: .inverse) ?? false
        widthScale = try container.decodeIfPresent(Int.self, forKey: .widthScale) ?? 1
        heightScale = try container.decodeIfPresent(Int.self, forKey: .heightScale) ?? 1
        align = try container.decodeIfPresent(TextAlign.self, forKey: .align) ?? .left
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        if bold { try container.encode(bold, forKey: .bold) }
        if underline != .none { try container.encode(underline, forKey: .underline) }
        if inverse { try container.encode(inverse, forKey: .inverse) }
        if widthScale != 1 { try container.encode(widthScale, forKey: .widthScale) }
        if heightScale != 1 { try container.encode(heightScale, forKey: .heightScale) }
        if align != .left { try container.encode(align, forKey: .align) }
    }
}

/// A block references a style by name or carries one inline
/// (docs/receipt-dsl.md, block table: "style ref or inline style").
enum StyleRef: Codable, Equatable, Hashable, Sendable {
    case named(String)
    case inline(TextStyle)

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if let name = try? container.decode(String.self) {
            self = .named(name)
        } else {
            self = .inline(try container.decode(TextStyle.self))
        }
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .named(let name): try container.encode(name)
        case .inline(let style): try container.encode(style)
        }
    }
}

// MARK: - Blocks

enum DividerStyle: String, Codable, CaseIterable, Sendable {
    case solid, dashed

    /// The character the hardware path fills the line with.
    var fillCharacter: Character {
        switch self {
        case .solid: return "-"
        case .dashed: return "·"
        }
    }

    var label: String {
        switch self {
        case .solid: return "Solid"
        case .dashed: return "Dashed"
        }
    }
}

enum QRErrorCorrection: String, Codable, CaseIterable, Sendable {
    case L, M, Q, H

    var label: String {
        switch self {
        case .L: return "L — 7%"
        case .M: return "M — 15%"
        case .Q: return "Q — 25%"
        case .H: return "H — 30%"
        }
    }
}

/// `width: chars | dots | flex(n)`. This example uses the two shapes the canonical
/// JSON in docs/receipt-dsl.md shows: the string "flex" and the object {"chars": n}.
enum CellWidth: Codable, Equatable, Hashable, Sendable {
    case flex
    case chars(Int)

    private enum CodingKeys: String, CodingKey { case chars }

    init(from decoder: Decoder) throws {
        if let single = try? decoder.singleValueContainer(),
           let word = try? single.decode(String.self) {
            guard word == "flex" else {
                throw DecodingError.dataCorruptedError(
                    in: single, debugDescription: "unknown cell width \"\(word)\"")
            }
            self = .flex
            return
        }
        let container = try decoder.container(keyedBy: CodingKeys.self)
        self = .chars(try container.decode(Int.self, forKey: .chars))
    }

    func encode(to encoder: Encoder) throws {
        switch self {
        case .flex:
            var container = encoder.singleValueContainer()
            try container.encode("flex")
        case .chars(let count):
            var container = encoder.container(keyedBy: CodingKeys.self)
            try container.encode(count, forKey: .chars)
        }
    }
}

struct ColumnCell: Codable, Equatable, Hashable, Sendable {
    var content: String
    var width: CellWidth
    var align: TextAlign?
    var style: StyleRef?

    private enum CodingKeys: String, CodingKey { case content, width, align, style }

    init(content: String, width: CellWidth, align: TextAlign? = nil, style: StyleRef? = nil) {
        self.content = content
        self.width = width
        self.align = align
        self.style = style
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        content = try container.decodeIfPresent(String.self, forKey: .content) ?? ""
        width = try container.decodeIfPresent(CellWidth.self, forKey: .width) ?? .flex
        align = try container.decodeIfPresent(TextAlign.self, forKey: .align)
        style = try container.decodeIfPresent(StyleRef.self, forKey: .style)
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        try container.encode(content, forKey: .content)
        try container.encode(width, forKey: .width)
        try container.encodeIfPresent(align, forKey: .align)
        try container.encodeIfPresent(style, forKey: .style)
    }
}

/// The block payloads. Serialization is by presence of the discriminating key, which
/// is what makes the JSON readable as a receipt rather than as a tagged union dump.
enum BlockContent: Equatable, Hashable, Sendable {
    case text(content: String, style: StyleRef?)
    case columns(cells: [ColumnCell])
    case divider(DividerStyle)
    case feed(lines: Int)
    case qr(payload: String, size: Int, ec: QRErrorCorrection, align: TextAlign)
}

/// One block plus a client-side identity. The identity is deliberately not part of
/// the document: it exists so SwiftUI can reorder rows, and two documents that
/// describe the same receipt must compare equal regardless of it.
struct ReceiptBlock: Identifiable, Codable, Equatable, Hashable, Sendable {
    let id: UUID
    var content: BlockContent

    init(id: UUID = UUID(), content: BlockContent) {
        self.id = id
        self.content = content
    }

    static func == (lhs: ReceiptBlock, rhs: ReceiptBlock) -> Bool {
        lhs.content == rhs.content
    }

    func hash(into hasher: inout Hasher) {
        hasher.combine(content)
    }

    private enum CodingKeys: String, CodingKey {
        case text, style
        case columns
        case divider
        case feed
        case qr, size, ec, align
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        id = UUID()

        if container.contains(.text) {
            content = .text(content: try container.decode(String.self, forKey: .text),
                            style: try container.decodeIfPresent(StyleRef.self, forKey: .style))
        } else if container.contains(.columns) {
            content = .columns(cells: try container.decode([ColumnCell].self, forKey: .columns))
        } else if container.contains(.divider) {
            content = .divider(try container.decode(DividerStyle.self, forKey: .divider))
        } else if container.contains(.feed) {
            content = .feed(lines: try container.decode(Int.self, forKey: .feed))
        } else if container.contains(.qr) {
            content = .qr(payload: try container.decode(String.self, forKey: .qr),
                          size: try container.decodeIfPresent(Int.self, forKey: .size) ?? 6,
                          ec: try container.decodeIfPresent(QRErrorCorrection.self, forKey: .ec) ?? .M,
                          align: try container.decodeIfPresent(TextAlign.self, forKey: .align) ?? .left)
        } else {
            throw DecodingError.dataCorrupted(
                .init(codingPath: container.codingPath,
                      debugDescription: "block carries no recognised key"))
        }
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        switch content {
        case .text(let text, let style):
            try container.encode(text, forKey: .text)
            try container.encodeIfPresent(style, forKey: .style)
        case .columns(let cells):
            try container.encode(cells, forKey: .columns)
        case .divider(let style):
            try container.encode(style, forKey: .divider)
        case .feed(let lines):
            try container.encode(lines, forKey: .feed)
        case .qr(let payload, let size, let ec, let align):
            try container.encode(payload, forKey: .qr)
            try container.encode(size, forKey: .size)
            try container.encode(ec, forKey: .ec)
            if align != .left { try container.encode(align, forKey: .align) }
        }
    }
}

// MARK: - Document

/// Media width comes from the printer at render time; the document declares content
/// (docs/receipt-dsl.md). This is the app's chosen preview width, not a document field.
enum PaperWidth: Int, Codable, CaseIterable, Identifiable, Sendable {
    case mm58 = 384
    case mm80 = 576

    var id: Int { rawValue }
    var widthDots: UInt32 { UInt32(rawValue) }

    /// Font A is 12 dots wide at 203 dpi: 384/12 = 32, 576/12 = 48.
    var charactersPerLine: Int {
        switch self {
        case .mm58: return 32
        case .mm80: return 48
        }
    }

    var label: String {
        switch self {
        case .mm58: return "58 mm"
        case .mm80: return "80 mm"
        }
    }

    var detailLabel: String { "\(label) · \(rawValue) dots · \(charactersPerLine) chars" }
}

struct ReceiptDocument: Codable, Equatable, Sendable {
    var version: Int
    var styles: [String: TextStyle]
    var blocks: [ReceiptBlock]

    private enum CodingKeys: String, CodingKey {
        case version = "v"
        case styles
        case blocks
    }

    init(version: Int = 1, styles: [String: TextStyle] = [:], blocks: [ReceiptBlock] = []) {
        self.version = version
        self.styles = styles
        self.blocks = blocks
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        version = try container.decodeIfPresent(Int.self, forKey: .version) ?? 1
        styles = try container.decodeIfPresent([String: TextStyle].self, forKey: .styles) ?? [:]
        blocks = try container.decodeIfPresent([ReceiptBlock].self, forKey: .blocks) ?? []
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        try container.encode(version, forKey: .version)
        try container.encode(styles, forKey: .styles)
        try container.encode(blocks, forKey: .blocks)
    }

    /// Resolves a block's style reference against the document's named styles.
    /// A name that is not in `styles` resolves to the default rather than failing:
    /// missing style, like a missing template path, is a declared fallback and
    /// never a crash mid-receipt.
    func resolve(_ ref: StyleRef?) -> TextStyle {
        switch ref {
        case .none: return styles["default"] ?? .default
        case .named(let name): return styles[name] ?? styles["default"] ?? .default
        case .inline(let style): return style
        }
    }

    var styleNames: [String] { styles.keys.sorted() }

    // MARK: JSON

    static func encoder(pretty: Bool) -> JSONEncoder {
        let encoder = JSONEncoder()
        encoder.outputFormatting = pretty
            ? [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
            : [.sortedKeys, .withoutEscapingSlashes]
        return encoder
    }

    func jsonData(pretty: Bool = true) throws -> Data {
        try ReceiptDocument.encoder(pretty: pretty).encode(self)
    }

    func jsonString(pretty: Bool = true) throws -> String {
        String(decoding: try jsonData(pretty: pretty), as: UTF8.self)
    }

    static func from(json: Data) throws -> ReceiptDocument {
        try JSONDecoder().decode(ReceiptDocument.self, from: json)
    }

    static func from(json: String) throws -> ReceiptDocument {
        try from(json: Data(json.utf8))
    }
}

// MARK: - Sample

extension ReceiptDocument {
    /// The worked example from docs/receipt-dsl.md, so a fresh install opens on
    /// something that exercises every block type the designer can produce.
    static var sample: ReceiptDocument {
        ReceiptDocument(
            version: 1,
            styles: [
                "default": TextStyle(),
                "h1": TextStyle(bold: true, widthScale: 2, heightScale: 2, align: .center),
                "muted": TextStyle(),
                "total": TextStyle(bold: true, heightScale: 2),
            ],
            blocks: [
                ReceiptBlock(content: .text(content: "CORNER CAFE", style: .named("h1"))),
                ReceiptBlock(content: .text(content: "Order 7F3A-92C1 · Table 4",
                                            style: .inline(TextStyle(align: .center)))),
                ReceiptBlock(content: .divider(.dashed)),
                ReceiptBlock(content: .columns(cells: [
                    ColumnCell(content: "2× Pilsner", width: .flex),
                    ColumnCell(content: "9.00", width: .chars(8), align: .right),
                ])),
                ReceiptBlock(content: .columns(cells: [
                    ColumnCell(content: "1× Goulash", width: .flex),
                    ColumnCell(content: "11.50", width: .chars(8), align: .right),
                ])),
                ReceiptBlock(content: .divider(.solid)),
                ReceiptBlock(content: .columns(cells: [
                    ColumnCell(content: "TOTAL", width: .flex, style: .named("total")),
                    ColumnCell(content: "20.50", width: .chars(8), align: .right,
                               style: .named("total")),
                ])),
                ReceiptBlock(content: .feed(lines: 1)),
                ReceiptBlock(content: .qr(payload: "7F3A-92C1", size: 6, ec: .M, align: .center)),
                ReceiptBlock(content: .text(content: "Thank you",
                                            style: .inline(TextStyle(align: .center)))),
                ReceiptBlock(content: .feed(lines: 2)),
            ])
    }
}
