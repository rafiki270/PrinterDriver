//
//  BlockEditorView.swift
//  ReceiptStudio
//
//  One block, edited in the terms the DSL uses: a style is either a reference to
//  one of the document's named styles or an inline override, and the editor makes
//  which of the two you are producing explicit — because the two serialise
//  differently and a document is meant to be readable afterwards.
//

import SwiftUI

struct BlockEditorView: View {

    @Binding var block: ReceiptBlock
    let styleNames: [String]

    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                switch block.content {
                case .text: textEditor
                case .columns: columnsEditor
                case .divider: dividerEditor
                case .feed: feedEditor
                case .qr: qrEditor
                }
            }
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }

    private var title: String {
        switch block.content {
        case .text: return "Text"
        case .columns: return "Columns"
        case .divider: return "Divider"
        case .feed: return "Feed"
        case .qr: return "QR code"
        }
    }

    // MARK: Text

    @ViewBuilder
    private var textEditor: some View {
        Section("Content") {
            TextField("Text", text: textContent, axis: .vertical)
                .lineLimit(1...6)
        }
        StyleRefEditor(ref: textStyleRef, styleNames: styleNames)
    }

    private var textContent: Binding<String> {
        Binding(
            get: {
                if case .text(let content, _) = block.content { return content }
                return ""
            },
            set: { value in
                if case .text(_, let style) = block.content {
                    block.content = .text(content: value, style: style)
                }
            })
    }

    private var textStyleRef: Binding<StyleRef?> {
        Binding(
            get: {
                if case .text(_, let style) = block.content { return style }
                return nil
            },
            set: { value in
                if case .text(let content, _) = block.content {
                    block.content = .text(content: content, style: value)
                }
            })
    }

    // MARK: Columns

    @ViewBuilder
    private var columnsEditor: some View {
        Section {
            TextField("Left cell", text: cellContent(0))
            TextField("Right cell", text: cellContent(1))
        } header: {
            Text("Cells")
        } footer: {
            Text("The left cell is flex: it takes whatever the right cell leaves. "
                 + "Rows are padded to the printer's characters per line here in the app, "
                 + "because the hardware path has no column command.")
        }

        Section("Right cell width") {
            Stepper(value: rightWidth, in: 2...20) {
                HStack {
                    Text("Characters")
                    Spacer()
                    Text("\(rightWidth.wrappedValue)")
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                }
            }
            Picker("Right cell align", selection: rightAlign) {
                ForEach(TextAlign.allCases, id: \.self) { option in
                    Text(option.label).tag(option)
                }
            }
        }

        StyleRefEditor(ref: rowStyleRef, styleNames: styleNames, title: "Row style")
    }

    private func cellContent(_ index: Int) -> Binding<String> {
        Binding(
            get: {
                guard case .columns(let cells) = block.content, cells.indices.contains(index) else {
                    return ""
                }
                return cells[index].content
            },
            set: { value in
                guard case .columns(var cells) = block.content, cells.indices.contains(index) else {
                    return
                }
                cells[index].content = value
                block.content = .columns(cells: cells)
            })
    }

    private var rightWidth: Binding<Int> {
        Binding(
            get: {
                guard case .columns(let cells) = block.content, cells.count > 1 else { return 8 }
                if case .chars(let count) = cells[1].width { return count }
                return 8
            },
            set: { value in
                guard case .columns(var cells) = block.content, cells.count > 1 else { return }
                cells[1].width = .chars(value)
                block.content = .columns(cells: cells)
            })
    }

    private var rightAlign: Binding<TextAlign> {
        Binding(
            get: {
                guard case .columns(let cells) = block.content, cells.count > 1 else { return .right }
                return cells[1].align ?? .right
            },
            set: { value in
                guard case .columns(var cells) = block.content, cells.count > 1 else { return }
                cells[1].align = value
                block.content = .columns(cells: cells)
            })
    }

    /// One style for the whole row, written onto every cell. The DSL allows a style
    /// per cell, but the hardware path prints one line with one set of GS ! and ESC E
    /// state, so a row whose cells disagreed could not be delivered as described.
    private var rowStyleRef: Binding<StyleRef?> {
        Binding(
            get: {
                guard case .columns(let cells) = block.content else { return nil }
                return cells.first?.style
            },
            set: { value in
                guard case .columns(var cells) = block.content else { return }
                for index in cells.indices {
                    cells[index].style = value
                }
                block.content = .columns(cells: cells)
            })
    }

    // MARK: Divider

    @ViewBuilder
    private var dividerEditor: some View {
        Section("Style") {
            Picker("Divider", selection: dividerStyle) {
                ForEach(DividerStyle.allCases, id: \.self) { option in
                    Text(option.label).tag(option)
                }
            }
            .pickerStyle(.inline)
            .labelsHidden()
        }
    }

    private var dividerStyle: Binding<DividerStyle> {
        Binding(
            get: {
                if case .divider(let style) = block.content { return style }
                return .solid
            },
            set: { block.content = .divider($0) })
    }

    // MARK: Feed

    @ViewBuilder
    private var feedEditor: some View {
        Section {
            Stepper(value: feedLines, in: 1...20) {
                HStack {
                    Text("Lines")
                    Spacer()
                    Text("\(feedLines.wrappedValue)")
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                }
            }
        } footer: {
            Text("ESC d n. The core adds its own final feed before the cut, so this is "
                 + "spacing inside the receipt, not the tail.")
        }
    }

    private var feedLines: Binding<Int> {
        Binding(
            get: {
                if case .feed(let lines) = block.content { return lines }
                return 1
            },
            set: { block.content = .feed(lines: $0) })
    }

    // MARK: QR

    @ViewBuilder
    private var qrEditor: some View {
        Section("Payload") {
            TextField("Payload", text: qrPayload, axis: .vertical)
                .lineLimit(1...4)
                .autocorrectionDisabled()
                .textInputAutocapitalization(.never)
                .monospaced()
        }

        Section {
            Stepper(value: qrSize, in: 1...16) {
                HStack {
                    Text("Module size")
                    Spacer()
                    Text("\(qrSize.wrappedValue)")
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                }
            }
            Picker("Error correction", selection: qrEC) {
                ForEach(QRErrorCorrection.allCases, id: \.self) { option in
                    Text(option.label).tag(option)
                }
            }
            Picker("Align", selection: qrAlign) {
                ForEach(TextAlign.allCases, id: \.self) { option in
                    Text(option.label).tag(option)
                }
            }
        } header: {
            Text("Symbol")
        } footer: {
            Text("The payload goes out as bytes, with no code-page transliteration: "
                 + "a QR payload is a byte string, not display text.")
        }
    }

    private var qrPayload: Binding<String> {
        Binding(
            get: {
                if case .qr(let payload, _, _, _) = block.content { return payload }
                return ""
            },
            set: { value in
                if case .qr(_, let size, let ec, let align) = block.content {
                    block.content = .qr(payload: value, size: size, ec: ec, align: align)
                }
            })
    }

    private var qrSize: Binding<Int> {
        Binding(
            get: {
                if case .qr(_, let size, _, _) = block.content { return size }
                return 6
            },
            set: { value in
                if case .qr(let payload, _, let ec, let align) = block.content {
                    block.content = .qr(payload: payload, size: value, ec: ec, align: align)
                }
            })
    }

    private var qrEC: Binding<QRErrorCorrection> {
        Binding(
            get: {
                if case .qr(_, _, let ec, _) = block.content { return ec }
                return .M
            },
            set: { value in
                if case .qr(let payload, let size, _, let align) = block.content {
                    block.content = .qr(payload: payload, size: size, ec: value, align: align)
                }
            })
    }

    private var qrAlign: Binding<TextAlign> {
        Binding(
            get: {
                if case .qr(_, _, _, let align) = block.content { return align }
                return .center
            },
            set: { value in
                if case .qr(let payload, let size, let ec, _) = block.content {
                    block.content = .qr(payload: payload, size: size, ec: ec, align: value)
                }
            })
    }
}

// MARK: - Style editing

/// Picks between the document's named styles and an inline override, and edits the
/// override in place. Switching to inline seeds it from whatever was showing, so
/// "start from h1 and nudge it" does not lose the heading.
struct StyleRefEditor: View {

    @Binding var ref: StyleRef?
    let styleNames: [String]
    var title: String = "Style"

    private enum Mode: Hashable {
        case documentDefault
        case named(String)
        case inline
    }

    private var mode: Binding<Mode> {
        Binding(
            get: {
                switch ref {
                case .none: return .documentDefault
                case .named(let name): return .named(name)
                case .inline: return .inline
                }
            },
            set: { newMode in
                switch newMode {
                case .documentDefault: ref = nil
                case .named(let name): ref = .named(name)
                case .inline: ref = .inline(inlineSeed)
                }
            })
    }

    private var inlineSeed: TextStyle {
        if case .inline(let style) = ref { return style }
        return TextStyle()
    }

    private var inlineStyle: Binding<TextStyle> {
        Binding(
            get: { inlineSeed },
            set: { ref = .inline($0) })
    }

    var body: some View {
        Section {
            Picker(title, selection: mode) {
                Text("Document default").tag(Mode.documentDefault)
                ForEach(styleNames, id: \.self) { name in
                    Text("Named: \(name)").tag(Mode.named(name))
                }
                Text("Inline (custom)").tag(Mode.inline)
            }
        } header: {
            Text(title)
        } footer: {
            Text("A named style serialises as \"style\": \"name\"; an inline style "
                 + "serialises as the style object itself.")
        }

        if case .inline = ref {
            TextStyleEditor(style: inlineStyle)
        }
    }
}

struct TextStyleEditor: View {

    @Binding var style: TextStyle

    var body: some View {
        Section("Emphasis") {
            Toggle("Bold", isOn: $style.bold)
            Toggle("Inverse (white on black)", isOn: $style.inverse)
            Picker("Underline", selection: $style.underline) {
                ForEach(UnderlineStyle.allCases, id: \.self) { option in
                    Text(option.label).tag(option)
                }
            }
        }

        Section {
            Stepper(value: $style.widthScale, in: 1...4) {
                HStack {
                    Text("Width scale")
                    Spacer()
                    Text("\(style.widthScale)×")
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                }
            }
            Stepper(value: $style.heightScale, in: 1...4) {
                HStack {
                    Text("Height scale")
                    Spacer()
                    Text("\(style.heightScale)×")
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                }
            }
            Picker("Align", selection: $style.align) {
                ForEach(TextAlign.allCases, id: \.self) { option in
                    Text(option.label).tag(option)
                }
            }
        } header: {
            Text("Size and position")
        } footer: {
            Text("GS ! multipliers. Doubling the width halves the characters per line, "
                 + "which the preview models.")
        }
    }
}
