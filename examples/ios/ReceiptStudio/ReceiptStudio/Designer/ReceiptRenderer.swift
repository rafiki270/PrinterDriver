//
//  ReceiptRenderer.swift
//  ReceiptStudio
//
//  Turns a ReceiptDocument into two things that must never disagree:
//
//    * `preview(...)` — what the on-screen paper shows,
//    * `operations(...)` — what the bridge hands the encoder.
//
//  Both come out of the same layout pass, so the preview is not a drawing of a
//  receipt, it is the receipt's own line breaking rendered on screen.
//
//  Columns are resolved here rather than in the bridge. docs/receipt-dsl.md
//  describes them as a layout facet (flex/chars widths, per-cell align), and the
//  hardware path has no column command: it has characters per line. Doing the
//  arithmetic where the preview is drawn is the only way the two can stay honest
//  about the same thing.
//

import Foundation

/// A transport-neutral op. The bridge translates these into PDOp; keeping the
/// layout output free of Objective-C types keeps it unit-testable on its own.
struct PrintOp: Equatable, Sendable {
    enum Kind: Equatable, Sendable {
        case text(String)
        case feed(Int)
        case qr(payload: String, moduleSize: Int, ec: QRErrorCorrection)
    }

    var kind: Kind
    var style: TextStyle

    static func text(_ value: String, style: TextStyle = .default) -> PrintOp {
        PrintOp(kind: .text(value), style: style)
    }
}

/// One element of the on-screen paper.
enum PreviewElement: Identifiable, Equatable {
    case line(id: UUID, text: String, style: TextStyle)
    case qr(id: UUID, payload: String, modules: Int, align: TextAlign)

    var id: UUID {
        switch self {
        case .line(let id, _, _): return id
        case .qr(let id, _, _, _): return id
        }
    }
}

enum ReceiptRenderer {

    // MARK: Line fitting

    /// Effective characters per line once the style's width multiplier is applied.
    /// GS ! doubles the glyph, so a 48-column receipt is a 24-column receipt in
    /// double width — a fact the preview has to model or every wide heading lies.
    static func columns(for style: TextStyle, width: PaperWidth) -> Int {
        max(1, width.charactersPerLine / max(1, style.widthScale))
    }

    /// Greedy word wrap, falling back to a hard break for a single word longer than
    /// the line. Empty input yields one empty line so a blank text block still
    /// advances the paper.
    static func wrap(_ text: String, to columns: Int) -> [String] {
        guard columns > 0 else { return [text] }
        guard !text.isEmpty else { return [""] }

        var lines: [String] = []
        for paragraph in text.components(separatedBy: "\n") {
            if paragraph.isEmpty {
                lines.append("")
                continue
            }
            var current = ""
            for word in paragraph.split(separator: " ", omittingEmptySubsequences: true) {
                var candidate = String(word)
                if candidate.count > columns {
                    // A single token that cannot fit: break it at the column edge
                    // rather than letting the printer decide silently.
                    if !current.isEmpty {
                        lines.append(current)
                        current = ""
                    }
                    while candidate.count > columns {
                        lines.append(String(candidate.prefix(columns)))
                        candidate = String(candidate.dropFirst(columns))
                    }
                    current = candidate
                    continue
                }
                if current.isEmpty {
                    current = candidate
                } else if current.count + 1 + candidate.count <= columns {
                    current += " " + candidate
                } else {
                    lines.append(current)
                    current = candidate
                }
            }
            lines.append(current)
        }
        return lines.isEmpty ? [""] : lines
    }

    static func pad(_ text: String, to width: Int, align: TextAlign) -> String {
        guard width > 0 else { return "" }
        let clipped = text.count > width ? String(text.prefix(width)) : text
        let slack = width - clipped.count
        guard slack > 0 else { return clipped }
        switch align {
        case .left:
            return clipped + String(repeating: " ", count: slack)
        case .right:
            return String(repeating: " ", count: slack) + clipped
        case .center:
            let leading = slack / 2
            return String(repeating: " ", count: leading)
                + clipped
                + String(repeating: " ", count: slack - leading)
        }
    }

    // MARK: Columns

    /// Lays a columns block out into one padded line of exactly `columns` characters.
    /// Fixed `chars` cells keep their width; `flex` cells share what is left, and a
    /// flex cell squeezed to nothing still gets one column so the row cannot vanish.
    static func layout(cells: [ColumnCell], columns: Int) -> String {
        guard !cells.isEmpty, columns > 0 else { return "" }

        var widths = [Int](repeating: 0, count: cells.count)
        var flexIndices: [Int] = []
        var fixedTotal = 0

        for (index, cell) in cells.enumerated() {
            switch cell.width {
            case .chars(let count):
                let clamped = max(0, min(count, columns))
                widths[index] = clamped
                fixedTotal += clamped
            case .flex:
                flexIndices.append(index)
            }
        }

        var remaining = max(0, columns - fixedTotal)
        if flexIndices.isEmpty {
            // No flex cell: pad the row out so the line still fills the paper.
            let rendered = zip(cells, widths).map { cell, width in
                pad(cell.content, to: width, align: cell.align ?? .left)
            }.joined()
            return pad(rendered, to: columns, align: .left)
        }

        let share = max(1, remaining / flexIndices.count)
        for (offset, index) in flexIndices.enumerated() {
            let isLast = offset == flexIndices.count - 1
            let width = isLast ? max(1, remaining) : min(share, remaining)
            widths[index] = width
            remaining -= width
        }

        let rendered = zip(cells, widths).map { cell, width in
            pad(cell.content, to: width, align: cell.align ?? .left)
        }.joined()
        return String(rendered.prefix(columns))
    }

    // MARK: Preview

    static func preview(_ document: ReceiptDocument, width: PaperWidth) -> [PreviewElement] {
        var elements: [PreviewElement] = []

        for block in document.blocks {
            switch block.content {
            case .text(let content, let styleRef):
                let style = document.resolve(styleRef)
                let columns = columns(for: style, width: width)
                for (index, line) in wrap(content, to: columns).enumerated() {
                    elements.append(.line(id: derive(block.id, index),
                                          text: pad(line, to: columns, align: style.align),
                                          style: style))
                }

            case .columns(let cells):
                let style = document.resolve(cells.first?.style)
                let columns = columns(for: style, width: width)
                elements.append(.line(id: block.id,
                                      text: layout(cells: cells, columns: columns),
                                      style: style))

            case .divider(let divider):
                let columns = width.charactersPerLine
                elements.append(.line(
                    id: block.id,
                    text: String(repeating: String(divider.fillCharacter), count: columns),
                    style: .default))

            case .feed(let lines):
                for index in 0..<max(1, lines) {
                    elements.append(.line(id: derive(block.id, index), text: "", style: .default))
                }

            case .qr(let payload, let size, _, let align):
                elements.append(.qr(id: block.id, payload: payload,
                                    modules: max(1, min(size, 16)), align: align))
            }
        }
        return elements
    }

    // MARK: Operations

    /// The same layout, expressed as ops for the document tier. Alignment is left to
    /// the printer (ESC a n) for text and QR, but columns arrive pre-padded, because
    /// a padded row that the printer then centres would be centred twice.
    static func operations(_ document: ReceiptDocument, width: PaperWidth) -> [PrintOp] {
        var ops: [PrintOp] = []

        for block in document.blocks {
            switch block.content {
            case .text(let content, let styleRef):
                let style = document.resolve(styleRef)
                for line in wrap(content, to: columns(for: style, width: width)) {
                    ops.append(.text(line, style: style))
                }

            case .columns(let cells):
                var style = document.resolve(cells.first?.style)
                let columns = columns(for: style, width: width)
                style.align = .left
                ops.append(.text(layout(cells: cells, columns: columns), style: style))

            case .divider(let divider):
                ops.append(.text(
                    String(repeating: String(divider.fillCharacter),
                           count: width.charactersPerLine),
                    style: .default))

            case .feed(let lines):
                ops.append(PrintOp(kind: .feed(max(1, min(lines, 255))), style: .default))

            case .qr(let payload, let size, let ec, let align):
                var style = TextStyle()
                style.align = align
                ops.append(PrintOp(
                    kind: .qr(payload: payload, moduleSize: max(1, min(size, 16)), ec: ec),
                    style: style))
            }
        }
        return ops
    }

    /// Stable per-line identity for a multi-line block, so SwiftUI does not rebuild
    /// the whole preview when one character changes.
    private static func derive(_ id: UUID, _ index: Int) -> UUID {
        var bytes = id.uuid
        bytes.15 = bytes.15 &+ UInt8(truncatingIfNeeded: index)
        bytes.14 = bytes.14 &+ UInt8(truncatingIfNeeded: index >> 8)
        return UUID(uuid: bytes)
    }
}
