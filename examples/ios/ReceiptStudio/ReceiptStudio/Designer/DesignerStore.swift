//
//  DesignerStore.swift
//  ReceiptStudio
//
//  The document being designed, persisted as its own canonical JSON.
//
//  Persisting the DSL form rather than some app-private encoding is the point:
//  what is on disk is exactly what Export copies out and exactly what a backend
//  would store (docs/receipt-dsl.md: "the canonical form is a plain data model").
//

import Foundation
import Observation

@MainActor
@Observable
final class DesignerStore {

    var document: ReceiptDocument {
        didSet { save() }
    }

    var width: PaperWidth {
        didSet { defaults.set(width.rawValue, forKey: widthKey) }
    }

    /// The last printer used, so Print does not re-ask every time.
    var lastPrinterId: UUID? {
        didSet {
            defaults.set(lastPrinterId?.uuidString, forKey: printerKey)
        }
    }

    private let documentKey = "receiptstudio.document.v1"
    private let widthKey = "receiptstudio.width.v1"
    private let printerKey = "receiptstudio.lastPrinter.v1"
    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults

        if let data = defaults.data(forKey: documentKey),
           let restored = try? ReceiptDocument.from(json: data) {
            document = restored
        } else {
            document = .sample
        }

        let storedWidth = defaults.integer(forKey: widthKey)
        width = PaperWidth(rawValue: storedWidth) ?? .mm80

        if let raw = defaults.string(forKey: printerKey) {
            lastPrinterId = UUID(uuidString: raw)
        }
    }

    private func save() {
        guard let data = try? document.jsonData(pretty: false) else { return }
        defaults.set(data, forKey: documentKey)
    }

    // MARK: Derived

    var preview: [PreviewElement] { ReceiptRenderer.preview(document, width: width) }

    var operations: [PrintOp] { ReceiptRenderer.operations(document, width: width) }

    var json: String {
        (try? document.jsonString(pretty: true)) ?? "{}"
    }

    // MARK: Block editing

    func append(_ content: BlockContent) {
        document.blocks.append(ReceiptBlock(content: content))
    }

    func delete(at offsets: IndexSet) {
        document.blocks.remove(atOffsets: offsets)
    }

    func move(from source: IndexSet, to destination: Int) {
        document.blocks.move(fromOffsets: source, toOffset: destination)
    }

    func replace(_ block: ReceiptBlock, with content: BlockContent) {
        guard let index = document.blocks.firstIndex(where: { $0.id == block.id }) else { return }
        document.blocks[index].content = content
    }

    func binding(for block: ReceiptBlock) -> Int? {
        document.blocks.firstIndex(where: { $0.id == block.id })
    }

    func resetToSample() {
        document = .sample
    }

    func clear() {
        document = ReceiptDocument(version: 1,
                                   styles: ["default": TextStyle()],
                                   blocks: [])
    }
}
