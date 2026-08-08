//
//  DesignerView.swift
//  ReceiptStudio
//

import SwiftUI

struct DesignerView: View {

    @Environment(DesignerStore.self) private var store
    @Environment(PrinterStore.self) private var printers

    @State private var editing: EditingBlock?
    @State private var showingJSON = false
    @State private var showingPrint = false

    struct EditingBlock: Identifiable {
        let id: UUID
    }

    var body: some View {
        @Bindable var store = store

        NavigationStack {
            VStack(spacing: 0) {
                ReceiptPreviewView(elements: store.preview, width: store.width)
                    .frame(height: 300)
                    .background(Color(.systemGroupedBackground))

                // Directly under the paper, because it is the one control that
                // changes what the paper shows: media width comes from the printer
                // at render time, and the preview has to be honest about which one
                // it is drawing (docs/receipt-dsl.md).
                HStack(spacing: 12) {
                    Picker("Paper width", selection: $store.width) {
                        ForEach(PaperWidth.allCases) { option in
                            Text(option.label).tag(option)
                        }
                    }
                    .pickerStyle(.segmented)
                    .frame(maxWidth: 180)

                    Text("\(store.width.rawValue) dots · \(store.width.charactersPerLine) chars")
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                    Spacer(minLength: 0)
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 8)
                .background(.bar)

                Divider()

                blockList
            }
            .navigationTitle("Designer")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    addMenu
                }
                ToolbarItem(placement: .topBarTrailing) {
                    EditButton()
                }
                ToolbarItemGroup(placement: .bottomBar) {
                    Button {
                        showingJSON = true
                    } label: {
                        Label("Export JSON", systemImage: "curlybraces")
                    }
                    Spacer()
                    Button {
                        showingPrint = true
                    } label: {
                        Label("Print", systemImage: "printer.fill")
                    }
                    .disabled(store.document.blocks.isEmpty)
                }
            }
            .sheet(item: $editing) { item in
                if let blockBinding = binding(for: item.id) {
                    BlockEditorView(block: blockBinding, styleNames: store.document.styleNames)
                }
            }
            .sheet(isPresented: $showingJSON) {
                JSONExportView(json: store.json)
            }
            .sheet(isPresented: $showingPrint) {
                PrintFlowView(operations: store.operations)
            }
        }
    }

    // MARK: Blocks

    private var blockList: some View {
        List {
            Section {
                ForEach(store.document.blocks) { block in
                    Button {
                        editing = EditingBlock(id: block.id)
                    } label: {
                        BlockRow(block: block, document: store.document)
                    }
                    .buttonStyle(.plain)
                }
                .onDelete { store.delete(at: $0) }
                .onMove { store.move(from: $0, to: $1) }

                if store.document.blocks.isEmpty {
                    ContentUnavailableView {
                        Label("Empty receipt", systemImage: "doc.plaintext")
                    } description: {
                        Text("Add a block to begin.")
                    }
                    .listRowBackground(Color.clear)
                }
            } header: {
                HStack {
                    Text("Blocks")
                    Spacer()
                    Text("\(store.document.blocks.count)")
                        .font(.caption.monospacedDigit())
                }
            } footer: {
                Text("Tap to edit, drag to reorder. The document serialises to the receipt "
                     + "DSL shape: { v, styles, blocks }.")
            }

            Section {
                Button("Load the sample receipt") { store.resetToSample() }
                Button("Clear all blocks", role: .destructive) { store.clear() }
            }
        }
        .listStyle(.insetGrouped)
    }

    private var addMenu: some View {
        Menu {
            Button {
                store.append(.text(content: "New line", style: nil))
            } label: {
                Label("Text", systemImage: "textformat")
            }
            Button {
                store.append(.columns(cells: [
                    ColumnCell(content: "Item", width: .flex),
                    ColumnCell(content: "0.00", width: .chars(8), align: .right),
                ]))
            } label: {
                Label("Columns", systemImage: "rectangle.split.2x1")
            }
            Button {
                store.append(.divider(.dashed))
            } label: {
                Label("Divider", systemImage: "minus")
            }
            Button {
                store.append(.feed(lines: 1))
            } label: {
                Label("Feed", systemImage: "arrow.down.to.line")
            }
            Button {
                store.append(.qr(payload: UUID().uuidString.prefix(8).description,
                                 size: 6, ec: .M, align: .center))
            } label: {
                Label("QR code", systemImage: "qrcode")
            }
        } label: {
            Label("Add block", systemImage: "plus")
        }
    }

    private func binding(for id: UUID) -> Binding<ReceiptBlock>? {
        guard store.document.blocks.contains(where: { $0.id == id }) else { return nil }
        return Binding(
            get: {
                store.document.blocks.first(where: { $0.id == id })
                    ?? ReceiptBlock(content: .text(content: "", style: nil))
            },
            set: { updated in
                guard let index = store.document.blocks.firstIndex(where: { $0.id == id }) else {
                    return
                }
                store.document.blocks[index] = updated
            })
    }
}

// MARK: - Rows

private struct BlockRow: View {
    let block: ReceiptBlock
    let document: ReceiptDocument

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: symbol)
                .frame(width: 22)
                .foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.body)
                    .lineLimit(1)
                Text(subtitle)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer()
            Image(systemName: "chevron.right")
                .font(.caption)
                .foregroundStyle(.tertiary)
        }
        .padding(.vertical, 2)
        .contentShape(Rectangle())
    }

    private var symbol: String {
        switch block.content {
        case .text: return "textformat"
        case .columns: return "rectangle.split.2x1"
        case .divider: return "minus"
        case .feed: return "arrow.down.to.line"
        case .qr: return "qrcode"
        }
    }

    private var title: String {
        switch block.content {
        case .text(let content, _):
            return content.isEmpty ? "(empty line)" : content
        case .columns(let cells):
            return cells.map(\.content).joined(separator: "   ")
        case .divider(let style):
            return "\(style.label) divider"
        case .feed(let lines):
            return "Feed \(lines) line\(lines == 1 ? "" : "s")"
        case .qr(let payload, _, _, _):
            return payload
        }
    }

    private var subtitle: String {
        switch block.content {
        case .text(_, let ref):
            return "text · " + styleDescription(ref)
        case .columns(let cells):
            let widths = cells.map { cell -> String in
                switch cell.width {
                case .flex: return "flex"
                case .chars(let n): return "\(n) ch"
                }
            }
            return "columns · " + widths.joined(separator: " + ")
        case .divider:
            return "divider"
        case .feed:
            return "feed"
        case .qr(_, let size, let ec, let align):
            return "qr · size \(size) · ec \(ec.rawValue) · \(align.rawValue)"
        }
    }

    private func styleDescription(_ ref: StyleRef?) -> String {
        switch ref {
        case .none:
            return "default style"
        case .named(let name):
            return "style \"\(name)\""
        case .inline(let style):
            var parts: [String] = []
            if style.bold { parts.append("bold") }
            if style.underline != .none { parts.append(style.underline.rawValue) }
            if style.inverse { parts.append("inverse") }
            if style.widthScale != 1 || style.heightScale != 1 {
                parts.append("\(style.widthScale)×\(style.heightScale)")
            }
            parts.append(style.align.rawValue)
            return "inline · " + parts.joined(separator: ", ")
        }
    }
}

// MARK: - Export

struct JSONExportView: View {

    let json: String
    @Environment(\.dismiss) private var dismiss
    @State private var copied = false

    var body: some View {
        NavigationStack {
            ScrollView([.vertical, .horizontal]) {
                Text(json)
                    .font(.system(size: 12, design: .monospaced))
                    .textSelection(.enabled)
                    .padding()
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .navigationTitle("Document JSON")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
                ToolbarItem(placement: .primaryAction) {
                    Button {
                        UIPasteboard.general.string = json
                        copied = true
                    } label: {
                        Label(copied ? "Copied" : "Copy", systemImage: copied ? "checkmark" : "doc.on.doc")
                    }
                }
                ToolbarItem(placement: .status) {
                    ShareLink(item: json) {
                        Label("Share", systemImage: "square.and.arrow.up")
                    }
                }
            }
        }
    }
}
