//
//  ReceiptPreviewView.swift
//  ReceiptStudio
//
//  On-screen paper. Monospaced, at exactly the characters-per-line the chosen
//  width gives (32 at 58 mm, 48 at 80 mm), with GS ! width multipliers modelled:
//  a double-width heading really does get half as many columns.
//
//  It is an approximation and says so. The exact preview is the raster path in
//  docs/receipt-dsl.md — render the document to an image and show that — which
//  this example does not implement.
//

import SwiftUI

struct ReceiptPreviewView: View {

    let elements: [PreviewElement]
    let width: PaperWidth

    /// SF Mono's advance is 0.6 em, so a line of N characters is 0.6 · N · size wide.
    private let advanceRatio: CGFloat = 0.6

    var body: some View {
        GeometryReader { geometry in
            let paperWidth = geometry.size.width - 24
            let fontSize = max(5, paperWidth / (CGFloat(width.charactersPerLine) * advanceRatio))
            let lineHeight = fontSize * 1.28

            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    ForEach(elements) { element in
                        switch element {
                        case .line(_, let text, let style):
                            PreviewLine(text: text,
                                        style: style,
                                        fontSize: fontSize,
                                        lineHeight: lineHeight)
                        case .qr(_, let payload, let modules, let align):
                            PreviewQR(payload: payload,
                                      modules: modules,
                                      align: align,
                                      paperWidth: paperWidth,
                                      fontSize: fontSize)
                        }
                    }
                    TornEdge()
                        .fill(Color(.systemBackground))
                        .frame(height: 10)
                        .padding(.top, 6)
                }
                .frame(width: paperWidth, alignment: .leading)
                .padding(.vertical, 14)
                .background(paper)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 8)
            }
        }
    }

    private var paper: some View {
        RoundedRectangle(cornerRadius: 3)
            .fill(Color(.secondarySystemGroupedBackground))
            .shadow(color: .black.opacity(0.12), radius: 4, y: 2)
            .padding(.horizontal, -12)
    }
}

private struct PreviewLine: View {
    let text: String
    let style: TextStyle
    let fontSize: CGFloat
    let lineHeight: CGFloat

    var body: some View {
        Text(text.isEmpty ? " " : text)
            .font(.system(size: fontSize, weight: style.bold ? .bold : .regular,
                          design: .monospaced))
            .underline(style.underline != .none)
            .foregroundStyle(style.inverse ? Color(.systemBackground) : Color.primary)
            .background(style.inverse ? Color.primary : Color.clear)
            .fixedSize(horizontal: true, vertical: true)
            .scaleEffect(x: CGFloat(style.widthScale),
                         y: CGFloat(style.heightScale),
                         anchor: .topLeading)
            .frame(height: lineHeight * CGFloat(style.heightScale), alignment: .topLeading)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// A stand-in, not a code. The bytes that reach the printer are GS ( k function
/// 180/181 with the real payload; drawing a scannable symbol on screen would need
/// an encoder this example deliberately does not carry.
private struct PreviewQR: View {
    let payload: String
    let modules: Int
    let align: TextAlign
    let paperWidth: CGFloat
    let fontSize: CGFloat

    private var side: CGFloat {
        min(paperWidth * 0.55, CGFloat(modules) * 14)
    }

    var body: some View {
        HStack(spacing: 0) {
            if align != .left { Spacer(minLength: 0) }
            VStack(spacing: 3) {
                Canvas { context, size in
                    let grid = 21
                    let cell = size.width / CGFloat(grid)
                    let bits = QRSketch.bits(for: payload, grid: grid)
                    for row in 0..<grid {
                        for column in 0..<grid {
                            guard bits[row * grid + column] else { continue }
                            let rect = CGRect(x: CGFloat(column) * cell,
                                              y: CGFloat(row) * cell,
                                              width: cell, height: cell)
                            context.fill(Path(rect), with: .color(.primary))
                        }
                    }
                }
                .frame(width: side, height: side)

                Text("QR · size \(modules) · approximation")
                    .font(.system(size: max(6, fontSize * 0.75), design: .monospaced))
                    .foregroundStyle(.secondary)
            }
            if align != .right { Spacer(minLength: 0) }
        }
        .padding(.vertical, 6)
    }
}

/// Deterministic pattern: three finder squares plus payload-derived noise, so the
/// same payload always draws the same block and a changed payload visibly changes it.
enum QRSketch {
    static func bits(for payload: String, grid: Int) -> [Bool] {
        var bits = [Bool](repeating: false, count: grid * grid)

        func finder(atRow originRow: Int, column originColumn: Int) {
            for row in 0..<7 {
                for column in 0..<7 {
                    let edge = row == 0 || row == 6 || column == 0 || column == 6
                    let core = (2...4).contains(row) && (2...4).contains(column)
                    guard edge || core else { continue }
                    let r = originRow + row
                    let c = originColumn + column
                    guard r < grid, c < grid else { continue }
                    bits[r * grid + c] = true
                }
            }
        }

        finder(atRow: 0, column: 0)
        finder(atRow: 0, column: grid - 7)
        finder(atRow: grid - 7, column: 0)

        // Timing rows, as on a real symbol.
        for index in 8..<(grid - 8) where index % 2 == 0 {
            bits[6 * grid + index] = true
            bits[index * grid + 6] = true
        }

        var hash: UInt64 = 0xcbf2_9ce4_8422_2325
        for byte in Array(payload.utf8) + [0x5a] {
            hash = (hash ^ UInt64(byte)) &* 0x0000_0100_0000_01b3
        }

        for row in 0..<grid {
            for column in 0..<grid {
                let index = row * grid + column
                guard !bits[index] else { continue }
                let inFinder = (row < 8 && column < 8)
                    || (row < 8 && column >= grid - 8)
                    || (row >= grid - 8 && column < 8)
                guard !inFinder, row != 6, column != 6 else { continue }
                hash = hash &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
                bits[index] = (hash >> 33) & 1 == 1
            }
        }
        return bits
    }
}

/// The zigzag a thermal cutter leaves, so the preview reads as paper at a glance.
struct TornEdge: Shape {
    var toothWidth: CGFloat = 9

    func path(in rect: CGRect) -> Path {
        var path = Path()
        path.move(to: CGPoint(x: rect.minX, y: rect.minY))
        var x = rect.minX
        var up = false
        while x < rect.maxX {
            let next = min(x + toothWidth, rect.maxX)
            path.addLine(to: CGPoint(x: next, y: up ? rect.minY : rect.maxY))
            up.toggle()
            x = next
        }
        path.addLine(to: CGPoint(x: rect.maxX, y: rect.minY))
        path.closeSubpath()
        return path
    }
}
