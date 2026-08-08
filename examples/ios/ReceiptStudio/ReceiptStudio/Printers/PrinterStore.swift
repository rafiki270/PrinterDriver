//
//  PrinterStore.swift
//  ReceiptStudio
//
//  Remembered printers (UserDefaults + Codable) and their liveness.
//
//  Liveness comes from the bridge's status query, not from a ping: docs/api.md §8
//  is explicit that availability ping-polling is one of the things the SDK replaces.
//  The dot is green only when a DLE EOT answer was actually decoded — a printer we
//  have never heard from shows as unknown rather than as healthy.
//

import Foundation
import Observation

/// What the row's dot is allowed to say.
enum Liveness: Equatable, Sendable {
    case unknown
    case checking
    case online
    case offline
    /// Reachable and answering, but the device reports a condition.
    case attention(String)
}

@MainActor
@Observable
final class PrinterStore {

    private(set) var printers: [SavedPrinter] = []
    private(set) var liveness: [UUID: Liveness] = [:]
    private(set) var lastDeviceEvent: [String: String] = [:]

    private let defaultsKey = "receiptstudio.printers.v1"
    private let defaults: UserDefaults
    private var pollTask: Task<Void, Never>?

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        load()
        PrinterDriverService.shared.subscribeDeviceEvents { [weak self] printerId, label in
            self?.lastDeviceEvent[printerId] = label
        }
    }

    // MARK: Persistence

    private func load() {
        guard let data = defaults.data(forKey: defaultsKey) else { return }
        printers = (try? JSONDecoder().decode([SavedPrinter].self, from: data)) ?? []
        for printer in printers {
            PrinterDriverService.shared.register(printer)
            liveness[printer.id] = .unknown
        }
    }

    private func save() {
        guard let data = try? JSONEncoder().encode(printers) else { return }
        defaults.set(data, forKey: defaultsKey)
    }

    // MARK: Mutation

    func contains(host: String, port: UInt16 = 9100) -> Bool {
        printers.contains { $0.host == host && $0.port == port }
    }

    @discardableResult
    func add(_ printer: SavedPrinter) -> SavedPrinter {
        if let existing = printers.first(where: { $0.host == printer.host && $0.port == printer.port }) {
            return existing
        }
        printers.append(printer)
        liveness[printer.id] = .unknown
        PrinterDriverService.shared.register(printer)
        save()
        Task { await refresh(printer) }
        return printer
    }

    func update(_ printer: SavedPrinter) {
        guard let index = printers.firstIndex(where: { $0.id == printer.id }) else { return }
        printers[index] = printer
        PrinterDriverService.shared.register(printer)
        save()
    }

    func remove(at offsets: IndexSet) {
        for index in offsets {
            liveness[printers[index].id] = nil
        }
        printers.remove(atOffsets: offsets)
        save()
    }

    func remove(_ printer: SavedPrinter) {
        printers.removeAll { $0.id == printer.id }
        liveness[printer.id] = nil
        save()
    }

    func printer(withId id: UUID) -> SavedPrinter? {
        printers.first { $0.id == id }
    }

    // MARK: Liveness

    func liveness(for printer: SavedPrinter) -> Liveness {
        liveness[printer.id] ?? .unknown
    }

    func refresh(_ printer: SavedPrinter) async {
        liveness[printer.id] = .checking
        let status = await PrinterDriverService.shared.refreshStatus(for: printer)
        liveness[printer.id] = Self.classify(status)
    }

    func refreshAll() async {
        for printer in printers {
            await refresh(printer)
        }
    }

    /// Background polling while the printers tab is on screen. Sequential on purpose:
    /// each status is a queued round trip on that printer's own connection, and
    /// firing them all at once only queues them behind each other anyway.
    func startPolling(every seconds: UInt64 = 20) {
        stopPolling()
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                await self?.refreshAll()
                try? await Task.sleep(for: .seconds(seconds))
            }
        }
    }

    func stopPolling() {
        pollTask?.cancel()
        pollTask = nil
    }

    static func classify(_ status: PDDeviceStatus) -> Liveness {
        guard status.connected else { return .offline }
        // Connected but nothing decoded yet: that is not evidence of health.
        guard status.observed else { return .unknown }
        if status.paperOut?.boolValue == true { return .attention("Paper out") }
        if status.coverOpen?.boolValue == true { return .attention("Cover open") }
        if status.cutterError?.boolValue == true { return .attention("Cutter error") }
        if status.unrecoverableError?.boolValue == true { return .attention("Hardware error") }
        if status.recoverableError?.boolValue == true { return .attention("Recoverable error") }
        if status.paperNearEnd?.boolValue == true { return .attention("Paper near end") }
        if status.online?.boolValue == false { return .offline }
        return .online
    }
}

extension PrinterDriverService {
    /// Device events (online/offline, paper, cover, cutter) for every configured
    /// printer, already on the main queue.
    func subscribeDeviceEvents(_ handler: @escaping @MainActor (String, String) -> Void) {
        PDBridge.shared().subscribeDeviceEvents { @Sendable printerId, event in
            MainActor.assumeIsolated { handler(printerId, event.label) }
        }
    }
}
