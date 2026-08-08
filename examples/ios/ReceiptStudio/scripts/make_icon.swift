#!/usr/bin/env swift
//
//  make_icon.swift
//  ReceiptStudio
//
//  Draws the app icon: a paper receipt with a torn bottom edge on a warm
//  background. CoreGraphics only, no dependencies, no design tool in the loop.
//
//  Run it from anywhere:
//
//      swift examples/ios/ReceiptStudio/scripts/make_icon.swift
//
//  Output (overwritten in place, so re-running is safe):
//
//      ReceiptStudio/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png
//
//  Pass a path as the first argument to write somewhere else.
//

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

// MARK: - Geometry

let side: CGFloat = 1024

// The paper, before the teeth are cut.
let paperX: CGFloat = 254
let paperWidth: CGFloat = 516
let paperTop: CGFloat = 116
let paperBody: CGFloat = 762          // straight part of the paper
let toothHeight: CGFloat = 40
let toothWidth: CGFloat = 43          // 516 / 12 teeth

let inset: CGFloat = 46
let contentX = paperX + inset
let contentWidth = paperWidth - inset * 2

// MARK: - Colours

func rgb(_ red: Int, _ green: Int, _ blue: Int, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat(red) / 255, green: CGFloat(green) / 255,
            blue: CGFloat(blue) / 255, alpha: alpha)
}

let backgroundTop = rgb(244, 168, 92)
let backgroundBottom = rgb(206, 84, 42)
let paperWhite = rgb(253, 251, 247)
let inkDark = rgb(46, 42, 40)
let inkMid = rgb(122, 116, 112)
let inkLight = rgb(178, 172, 167)

// MARK: - Drawing helpers

func roundedBar(_ context: CGContext, x: CGFloat, y: CGFloat, width: CGFloat,
                height: CGFloat, color: CGColor) {
    let rect = CGRect(x: x, y: y, width: width, height: height)
    let path = CGPath(roundedRect: rect,
                      cornerWidth: min(height / 2, 8),
                      cornerHeight: min(height / 2, 8),
                      transform: nil)
    context.setFillColor(color)
    context.addPath(path)
    context.fillPath()
}

func centeredBar(_ context: CGContext, y: CGFloat, width: CGFloat, height: CGFloat,
                 color: CGColor) {
    roundedBar(context, x: paperX + (paperWidth - width) / 2, y: y,
               width: width, height: height, color: color)
}

/// The paper outline: straight sides, flat top, zigzag along the bottom.
/// Coordinates are top-down; the context is flipped before any of this runs.
func paperPath() -> CGPath {
    let path = CGMutablePath()
    path.move(to: CGPoint(x: paperX, y: paperTop))
    path.addLine(to: CGPoint(x: paperX + paperWidth, y: paperTop))
    path.addLine(to: CGPoint(x: paperX + paperWidth, y: paperTop + paperBody))

    var x = paperX + paperWidth
    var down = true
    while x > paperX {
        let next = max(x - toothWidth, paperX)
        let y = paperTop + paperBody + (down ? toothHeight : 0)
        path.addLine(to: CGPoint(x: next, y: y))
        down.toggle()
        x = next
    }

    path.addLine(to: CGPoint(x: paperX, y: paperTop))
    path.closeSubpath()
    return path
}

/// A 9×9 stand-in with the three finder squares a real symbol has. It is a logo,
/// not a code, and nothing in the app pretends otherwise.
let qrPattern: [String] = [
    "111010111",
    "101000101",
    "111010111",
    "000101000",
    "011010110",
    "000101000",
    "111011010",
    "101000101",
    "111010111",
]

// MARK: - Render

func drawIcon(into context: CGContext) {
    // Flip to top-left origin so the layout above reads like a receipt.
    context.translateBy(x: 0, y: side)
    context.scaleBy(x: 1, y: -1)

    // Background: warm gradient. The base fill matters — an icon has no
    // transparency to fall back on, and any pixel the gradient misses is black.
    let canvas = CGRect(x: 0, y: 0, width: side, height: side)
    context.setFillColor(backgroundBottom)
    context.fill(canvas)

    let colorSpace = CGColorSpaceCreateDeviceRGB()
    if let gradient = CGGradient(colorsSpace: colorSpace,
                                 colors: [backgroundTop, backgroundBottom] as CFArray,
                                 locations: [0, 1]) {
        context.saveGState()
        context.addRect(canvas)
        context.clip()
        context.drawLinearGradient(gradient,
                                   start: CGPoint(x: side * 0.1, y: 0),
                                   end: CGPoint(x: side * 0.75, y: side),
                                   options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
        context.restoreGState()
    }

    // A soft highlight behind the paper, so the warm field is not flat.
    context.saveGState()
    context.setFillColor(rgb(255, 255, 255, 0.10))
    context.fillEllipse(in: CGRect(x: -120, y: -180, width: 900, height: 900))
    context.restoreGState()

    // The paper, with a subtle drop shadow.
    context.saveGState()
    context.setShadow(offset: CGSize(width: 0, height: 18),
                      blur: 34,
                      color: rgb(80, 26, 8, 0.35))
    context.setFillColor(paperWhite)
    context.addPath(paperPath())
    context.fillPath()
    context.restoreGState()

    // Everything else is clipped to the paper so no bar can overhang a tooth.
    context.saveGState()
    context.addPath(paperPath())
    context.clip()

    // Heading: one bold bar and a lighter subtitle.
    centeredBar(context, y: 186, width: 296, height: 40, color: inkDark)
    centeredBar(context, y: 250, width: 196, height: 20, color: inkLight)

    // Dashed divider.
    var dash = contentX
    while dash < contentX + contentWidth - 8 {
        roundedBar(context, x: dash, y: 302, width: 16, height: 8, color: inkLight)
        dash += 28
    }

    // Item rows: description on the left, amount on the right.
    let rowWidths: [CGFloat] = [252, 214, 236]
    for (index, width) in rowWidths.enumerated() {
        let y = 344 + CGFloat(index) * 46
        roundedBar(context, x: contentX, y: y, width: width, height: 18, color: inkMid)
        roundedBar(context, x: contentX + contentWidth - 92, y: y,
                   width: 92, height: 18, color: inkMid)
    }

    // Solid divider, then the total in heavier ink.
    roundedBar(context, x: contentX, y: 494, width: contentWidth, height: 8, color: inkDark)
    roundedBar(context, x: contentX, y: 526, width: 128, height: 26, color: inkDark)
    roundedBar(context, x: contentX + contentWidth - 148, y: 526,
               width: 148, height: 26, color: inkDark)

    // The QR block.
    let qrSide: CGFloat = 176
    let module = qrSide / CGFloat(qrPattern.count)
    let qrX = paperX + (paperWidth - qrSide) / 2
    let qrY: CGFloat = 596
    context.setFillColor(inkDark)
    for (row, line) in qrPattern.enumerated() {
        for (column, character) in line.enumerated() where character == "1" {
            context.fill(CGRect(x: qrX + CGFloat(column) * module,
                                y: qrY + CGFloat(row) * module,
                                width: module, height: module))
        }
    }

    // A closing line under the code.
    centeredBar(context, y: 800, width: 168, height: 16, color: inkLight)

    context.restoreGState()
}

// MARK: - Output

func destinationURL() -> URL {
    if CommandLine.arguments.count > 1 {
        return URL(fileURLWithPath: CommandLine.arguments[1])
    }
    let scriptDirectory = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    return scriptDirectory
        .deletingLastPathComponent()
        .appendingPathComponent("ReceiptStudio/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png")
        .standardizedFileURL
}

guard let context = CGContext(data: nil,
                              width: Int(side),
                              height: Int(side),
                              bitsPerComponent: 8,
                              bytesPerRow: 0,
                              space: CGColorSpaceCreateDeviceRGB(),
                              bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else {
    FileHandle.standardError.write(Data("make_icon: could not create a bitmap context\n".utf8))
    exit(1)
}

context.setAllowsAntialiasing(true)
context.interpolationQuality = .high
drawIcon(into: context)

guard let image = context.makeImage() else {
    FileHandle.standardError.write(Data("make_icon: could not render the image\n".utf8))
    exit(1)
}

let output = destinationURL()
try? FileManager.default.createDirectory(at: output.deletingLastPathComponent(),
                                         withIntermediateDirectories: true)

guard let destination = CGImageDestinationCreateWithURL(output as CFURL,
                                                        UTType.png.identifier as CFString,
                                                        1, nil) else {
    FileHandle.standardError.write(Data("make_icon: could not open \(output.path)\n".utf8))
    exit(1)
}

CGImageDestinationAddImage(destination, image, nil)
guard CGImageDestinationFinalize(destination) else {
    FileHandle.standardError.write(Data("make_icon: could not write \(output.path)\n".utf8))
    exit(1)
}

let attributes = try? FileManager.default.attributesOfItem(atPath: output.path)
let bytes = (attributes?[.size] as? Int) ?? 0
print("make_icon: wrote \(output.path) (\(Int(side))×\(Int(side)), \(bytes) bytes)")
