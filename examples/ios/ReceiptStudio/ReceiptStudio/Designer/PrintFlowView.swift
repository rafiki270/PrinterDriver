//
//  PrintFlowView.swift
//  ReceiptStudio
//
//  Pick a printer, send, and watch the job.
//
//  The terminal screen is the whole reason this example exists. A job ends done,
//  failed, or unknown (docs/api.md §4), and this view renders three outcomes, not
//  two-and-a-rounding-error:
//
//    done    — green, plus what the claim rests on: ConfidenceLevel, the grade
//              letter and who is making the claim. "Done, grade E, transport only"
//              is a different sentence from "Done, grade A, physical printer" and
//              the operator gets to read both.
//    failed  — red, with the FailureReason. Nothing printed, or the failure was
//              confirmed; the same key is safe to resubmit.
//    unknown — amber. Bytes were sent and nothing acknowledged them. It is not a
//              failure and it is not a success. The only honest thing the UI can
//              do is say so and hand the decision to a person, with forceReprint
//              available for when that person chooses it.
//

import SwiftUI

struct PrintFlowView: View {

    let operations: [PrintOp]

    @Environment(PrinterStore.self) private var printers
    @Environment(DesignerStore.self) private var designer
    @Environment(\.dismiss) private var dismiss

    @State private var job = PrintJobModel()
    @State private var chosen: SavedPrinter?

    var body: some View {
        NavigationStack {
            Group {
                if let chosen {
                    JobStatusView(job: job, printer: chosen, operations: operations)
                } else {
                    picker
                }
            }
            .navigationTitle(chosen == nil ? "Print to" : "Printing")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(job.isTerminal || chosen == nil ? "Close" : "Hide") { dismiss() }
                }
            }
        }
        .interactiveDismissDisabled(job.isRunning)
    }

    private var picker: some View {
        List {
            if printers.printers.isEmpty {
                ContentUnavailableView {
                    Label("No printers saved", systemImage: "printer")
                } description: {
                    Text("Add one in the Printers tab first.")
                }
            } else {
                Section {
                    ForEach(printers.printers) { printer in
                        Button {
                            chosen = printer
                            designer.lastPrinterId = printer.id
                            job.start(operations, on: printer)
                        } label: {
                            HStack(spacing: 12) {
                                LivenessDot(liveness: printers.liveness(for: printer))
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(printer.displayName)
                                        .foregroundStyle(.primary)
                                    Text("\(printer.endpoint) · \(printer.width.label)")
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                }
                                Spacer()
                            }
                        }
                    }
                } footer: {
                    Text("Every submission carries an idempotency key. Sending the same key "
                         + "again returns the existing job rather than printing twice.")
                }
            }
        }
    }
}

// MARK: - Job model

@MainActor
@Observable
final class PrintJobModel {

    private(set) var events: [PDJobEvent] = []
    private(set) var result: PDJobResult?
    private(set) var isRunning = false
    private(set) var key: String = ""
    private(set) var attempt = 0

    var isTerminal: Bool { result != nil }

    /// A fresh UUID per submission (docs/api.md §3: caller-supplied stable id).
    /// forceReprint deliberately reuses it — that is what makes the reprint a
    /// declared duplicate of a specific job rather than a new one.
    func start(_ operations: [PrintOp], on printer: SavedPrinter, forceReprint: Bool = false) {
        guard !isRunning else { return }
        if !forceReprint {
            key = "receiptstudio-" + UUID().uuidString.lowercased()
            events = []
        }
        result = nil
        isRunning = true
        attempt += 1

        PrinterDriverService.shared.submit(
            operations,
            to: printer,
            key: key,
            forceReprint: forceReprint,
            onProgress: { [weak self] event in
                self?.events.append(event)
            },
            completion: { [weak self] outcome in
                self?.result = outcome
                self?.isRunning = false
            })
    }

    func reset() {
        events = []
        result = nil
        isRunning = false
        attempt = 0
    }
}

// MARK: - Status

struct JobStatusView: View {

    @Bindable var job: PrintJobModel
    let printer: SavedPrinter
    let operations: [PrintOp]

    var body: some View {
        List {
            Section {
                StatusRow(label: "Printer", value: printer.endpoint)
                StatusRow(label: "Idempotency key", value: job.key)
                if job.attempt > 1 {
                    StatusRow(label: "Attempt", value: "\(job.attempt)", tint: .orange)
                }
            } header: {
                Text("Job")
            }

            Section("Progress") {
                if job.events.isEmpty {
                    HStack {
                        ProgressView()
                        Text("Submitting…").foregroundStyle(.secondary)
                    }
                }
                ForEach(Array(job.events.enumerated()), id: \.offset) { _, event in
                    JobEventRow(event: event)
                }
            }

            if let result = job.result {
                outcomeSection(result)
            }
        }
    }

    // MARK: Terminal

    @ViewBuilder
    private func outcomeSection(_ result: PDJobResult) -> some View {
        Section {
            HStack(spacing: 12) {
                Image(systemName: OutcomePalette.symbol(for: result.outcome))
                    .font(.title)
                    .foregroundStyle(OutcomePalette.color(for: result.outcome))
                VStack(alignment: .leading, spacing: 2) {
                    Text(OutcomePalette.title(for: result.outcome))
                        .font(.title3.weight(.semibold))
                        .foregroundStyle(OutcomePalette.color(for: result.outcome))
                    Text(headline(result))
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.vertical, 4)

            switch result.outcome {
            case .done:
                doneDetail(result)
            case .failed:
                failedDetail(result)
            case .unknown:
                unknownDetail(result)
            @unknown default:
                unknownDetail(result)
            }
        } header: {
            Text("Outcome")
        }
    }

    private func headline(_ result: PDJobResult) -> String {
        switch result.outcome {
        case .done:
            return "The printer acknowledged completion."
        case .failed:
            return result.reason.explanation.isEmpty
                ? "Confirmed failure — nothing printed."
                : result.reason.explanation
        case .unknown:
            return "May or may not have printed — operator decides."
        @unknown default:
            return "May or may not have printed — operator decides."
        }
    }

    @ViewBuilder
    private func doneDetail(_ result: PDJobResult) -> some View {
        StatusRow(label: "Evidence",
                  value: "\(result.confidence.label), grade \(result.grade.letter), "
                       + "\(result.authority.label)",
                  tint: .green)
        StatusRow(label: "Grade", value: "\(result.grade.letter) — \(result.grade.label)")
        StatusRow(label: "Method", value: result.method)
        Text("This is what the claim rests on. A grade E \"done\" means only that the "
             + "write succeeded; a grade A \"done\" means the printer itself confirmed "
             + "the job. The SDK never upgrades one into the other.")
            .font(.footnote)
            .foregroundStyle(.secondary)
    }

    @ViewBuilder
    private func failedDetail(_ result: PDJobResult) -> some View {
        StatusRow(label: "Reason", value: result.reason.label, tint: .red)
        StatusRow(label: "Reached", value: result.confidence.label)
        Text("Nothing printed, or the failure was confirmed. Resubmitting the same key "
             + "is safe.")
            .font(.footnote)
            .foregroundStyle(.secondary)

        Button {
            job.start(operations, on: printer, forceReprint: false)
        } label: {
            Label("Try again", systemImage: "arrow.clockwise")
        }
        .disabled(job.isRunning)
    }

    @ViewBuilder
    private func unknownDetail(_ result: PDJobResult) -> some View {
        StatusRow(label: "Reached", value: result.confidence.label, tint: .orange)
        StatusRow(label: "Reason", value: result.reason.label, tint: .orange)
        Text("Bytes were sent and nothing acknowledged them. This is not a failure and "
             + "it is not a success. Look at the paper, then decide.")
            .font(.footnote)
            .foregroundStyle(.secondary)

        Button {
            job.start(operations, on: printer, forceReprint: true)
        } label: {
            Label("Force reprint (deliberate duplicate)", systemImage: "doc.on.doc")
        }
        .disabled(job.isRunning)

        Text("A force reprint marks the ticket *** REPRINT / POSSIBLE DUPLICATE *** and "
             + "prints the attempt number, so whoever picks it up can tell.")
            .font(.caption)
            .foregroundStyle(.secondary)
    }
}

private struct JobEventRow: View {
    let event: PDJobEvent

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: symbol)
                .foregroundStyle(tint)
                .frame(width: 20)
            VStack(alignment: .leading, spacing: 2) {
                Text(event.state.label)
                    .font(.subheadline.weight(.medium))
                Text(event.state.detail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Text(event.confidence.label)
                .font(.caption2.monospaced())
                .foregroundStyle(.secondary)
        }
        .padding(.vertical, 1)
    }

    private var symbol: String {
        switch event.state {
        case .failedKnown: return "xmark.circle.fill"
        case .unknown: return "questionmark.circle.fill"
        case .doneSoftware, .physicallyVerified: return "checkmark.circle.fill"
        default: return "circle.fill"
        }
    }

    private var tint: Color {
        switch event.state {
        case .failedKnown: return .red
        case .unknown: return .orange
        case .doneSoftware, .physicallyVerified: return .green
        default: return .secondary.opacity(0.5)
        }
    }
}
