//
//  PrinterDriverService.swift
//  ReceiptStudio
//
//  Swift face of PDBridge. Everything here runs on the main actor because the
//  bridge already guarantees main-queue delivery for every callback; the only job
//  left is turning designer ops into PDOp and closures into async where it reads
//  better.
//
//  The send-with-closure shape is docs/api.md §12: `onProgress` receives every
//  JobEvent in order, the terminal closure fires exactly once, and both are sugar
//  over the one core event stream.
//

import Foundation

@MainActor
final class PrinterDriverService {

    static let shared = PrinterDriverService()

    private let bridge = PDBridge.shared()
    private var registered: Set<String> = []

    private init() {}

    var storeDirectory: String { bridge.storeDirectory }

    /// Device-database entries (docs/device-database.md), for the profile picker.
    static var profileNames: [String] { PDBridge.profileNames() }

    // MARK: Printers

    /// Idempotent: adding a printer that already exists returns the same id.
    @discardableResult
    func register(_ printer: SavedPrinter) -> String {
        let id = bridge.addPrinter(host: printer.host,
                                   port: printer.port,
                                   widthDots: printer.width.widthDots,
                                   profileId: printer.profileId)
        registered.insert(id)
        return id
    }

    func printerId(for printer: SavedPrinter) -> String {
        register(printer)
    }

    // MARK: Status

    /// Live DLE EOT 1-4 round trip, queued behind any active job.
    func refreshStatus(for printer: SavedPrinter) async -> PDDeviceStatus {
        let id = register(printer)
        return await withCheckedContinuation { continuation in
            bridge.refreshStatus(printerId: id) { @Sendable status in
                continuation.resume(returning: status)
            }
        }
    }

    /// Last decoded snapshot. Never a live query, so it is safe to call while a job
    /// is running — and it says `observed == false` when nothing has been heard yet
    /// rather than reporting healthy.
    func lastKnownStatus(for printer: SavedPrinter) -> PDDeviceStatus {
        bridge.lastKnownStatus(printerId: register(printer))
    }

    // MARK: Identify

    func identify(host: String, port: UInt16, mac: String? = nil) async throws -> PDIdentity {
        try await withCheckedThrowingContinuation { continuation in
            bridge.identify(host: host, port: port, mac: mac) { @Sendable identity, error in
                if let identity {
                    continuation.resume(returning: identity)
                } else {
                    continuation.resume(throwing: IdentifyError(message: error ?? "probe failed"))
                }
            }
        }
    }

    struct IdentifyError: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    // MARK: Printing

    /// The bridge guarantees both callbacks arrive on the main queue, so they are
    /// declared @MainActor here and the hop is asserted rather than re-dispatched:
    /// assumeIsolated traps if that guarantee is ever broken, which is a better
    /// outcome than a second async hop quietly reordering job events.
    func submit(_ ops: [PrintOp],
                to printer: SavedPrinter,
                key: String,
                forceReprint: Bool = false,
                onProgress: @escaping @MainActor (PDJobEvent) -> Void,
                completion: @escaping @MainActor (PDJobResult) -> Void) {
        let id = register(printer)
        let bridged = ops.map(Self.bridgeOp)

        let progress: @Sendable (PDJobEvent) -> Void = { event in
            MainActor.assumeIsolated { onProgress(event) }
        }
        let finished: @Sendable (PDJobResult) -> Void = { result in
            MainActor.assumeIsolated { completion(result) }
        }

        if forceReprint {
            bridge.forceReprint(ops: bridged, printerId: id, key: key,
                                progress: progress, completion: finished)
        } else {
            bridge.submit(ops: bridged, printerId: id, key: key,
                          progress: progress, completion: finished)
        }
    }

    private static func bridgeOp(_ op: PrintOp) -> PDOp {
        switch op.kind {
        case .text(let value):
            let bridged = PDOp.textOp(value)
            apply(op.style, to: bridged)
            return bridged

        case .feed(let lines):
            return PDOp.feedOp(UInt8(clamping: lines))

        case .qr(let payload, let moduleSize, let ec):
            return PDOp.qrOp(payload,
                             moduleSize: UInt8(clamping: moduleSize),
                             errorCorrection: ec.rawValue,
                             alignment: alignment(op.style.align))
        }
    }

    private static func apply(_ style: TextStyle, to op: PDOp) {
        op.alignment = alignment(style.align)
        op.bold = style.bold
        op.underline = style.underline.dots
        op.inverse = style.inverse
        op.widthScale = UInt8(clamping: style.widthScale)
        op.heightScale = UInt8(clamping: style.heightScale)
    }

    private static func alignment(_ align: TextAlign) -> PDAlignment {
        switch align {
        case .left: return .left
        case .center: return .center
        case .right: return .right
        }
    }
}

// MARK: - Presentation helpers for the closed enums
//
// Naming only. No case is merged with another, and there is deliberately no
// `isSuccess` anywhere: Unknown is its own outcome and stays visible as one
// (docs/api.md §1.4).

extension PDJobState {
    var label: String {
        switch self {
        case .queued: return "Queued"
        case .preflightOk: return "Preflight OK"
        case .sendStarted: return "Send started"
        case .bytesSent: return "Bytes sent"
        case .printConfirmed: return "Print confirmed"
        case .cutCommandProcessed: return "Cut processed"
        case .doneSoftware: return "Done (software)"
        case .physicallyVerified: return "Physically verified"
        case .failedKnown: return "Failed"
        case .unknown: return "Unknown"
        case .heldOffline: return "Held offline"
        @unknown default: return "Unknown"
        }
    }

    var detail: String {
        switch self {
        case .queued: return "Waiting for the printer's queue"
        case .preflightOk: return "Cover closed, paper present"
        case .sendStarted: return "Journal committed, bytes going out"
        case .bytesSent: return "Payload handed to the link"
        case .printConfirmed: return "The printer answered the ordered fence"
        case .cutCommandProcessed: return "The cut command was processed"
        case .doneSoftware: return "Completion acknowledged by the device"
        case .physicallyVerified: return "Verified by an out-of-band check"
        case .failedKnown: return "Confirmed failure"
        case .unknown: return "Bytes sent, nothing acknowledged them"
        case .heldOffline: return "Held by the queue addon"
        @unknown default: return ""
        }
    }
}

extension PDConfidenceLevel {
    var label: String {
        switch self {
        case .transportAccepted: return "TransportAccepted"
        case .printerHealthy: return "PrinterHealthy"
        case .printConfirmed: return "PrintConfirmed"
        case .cutProcessed: return "CutProcessed"
        case .cutFaultFree: return "CutFaultFree"
        case .physicallyVerified: return "PhysicallyVerified"
        @unknown default: return "TransportAccepted"
        }
    }
}

extension PDFailureReason {
    var label: String {
        switch self {
        case .none: return "None"
        case .transportUnreachable: return "TransportUnreachable"
        case .preflightCoverOpen: return "PreflightCoverOpen"
        case .preflightPaperOut: return "PreflightPaperOut"
        case .preflightHardwareError: return "PreflightHardwareError"
        case .timeoutAwaitingCompletion: return "TimeoutAwaitingCompletion"
        case .cutterFault: return "CutterFault"
        case .unsupported: return "Unsupported"
        case .unknown: return "Unknown"
        case .expired: return "Expired"
        case .queueOverflow: return "QueueOverflow"
        @unknown default: return "Unknown"
        }
    }

    var explanation: String {
        switch self {
        case .transportUnreachable: return "The printer could not be reached at that address."
        case .preflightCoverOpen: return "The cover was open, so nothing was sent."
        case .preflightPaperOut: return "There was no paper, so nothing was sent."
        case .preflightHardwareError: return "The printer reported a hardware error before sending."
        case .timeoutAwaitingCompletion: return "No acknowledgement arrived within the budget."
        case .cutterFault: return "The cutter reported a fault."
        case .unsupported: return "This printer's completion mechanism is not drivable here."
        default: return ""
        }
    }
}

extension PDConfidenceGrade {
    var letter: String {
        switch self {
        case .aJobLevelConfirmation: return "A"
        case .bOrderedDeviceResponse: return "B"
        case .cDeviceStatusAround: return "C"
        case .dSpoolerCompleted: return "D"
        case .eTransportOnly: return "E"
        @unknown default: return "E"
        }
    }

    var label: String {
        switch self {
        case .aJobLevelConfirmation: return "job-level confirmation"
        case .bOrderedDeviceResponse: return "ordered device response"
        case .cDeviceStatusAround: return "device status around the send"
        case .dSpoolerCompleted: return "spooler said completed"
        case .eTransportOnly: return "transport only"
        @unknown default: return "transport only"
        }
    }
}

extension PDCompletionAuthority {
    var label: String {
        switch self {
        case .physicalPrinter: return "physical printer"
        case .vendorSpooler: return "vendor spooler"
        case .pdAgent: return "agent"
        case .printServer: return "print server"
        case .transportOnly: return "transport only"
        @unknown default: return "transport only"
        }
    }
}

extension PDDeviceEvent {
    var label: String {
        switch self {
        case .online: return "Online"
        case .offline: return "Offline"
        case .coverOpen: return "Cover open"
        case .coverClosed: return "Cover closed"
        case .paperOut: return "Paper out"
        case .paperNearEnd: return "Paper near end"
        case .paperOk: return "Paper OK"
        case .cutterError: return "Cutter error"
        case .recoverableError: return "Recoverable error"
        case .unrecoverableError: return "Unrecoverable error"
        case .connectionLost: return "Connection lost"
        case .connectionRestored: return "Connection restored"
        @unknown default: return "Unknown"
        }
    }
}
