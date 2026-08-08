//
//  PrinterDetailView.swift
//  ReceiptStudio
//
//  A status snapshot and the capability probe.
//
//  Two things this screen refuses to do: report a flag it has not observed, and
//  present the identity guess as a fact. GS I can lie — Rongta's own manual
//  documents its printers answering as an Epson TM-T88V — so the vendor guess
//  arrives with a confidence percentage and a trusted flag, and both are shown
//  (docs/capability-profiles.md "Identification: multi-signal fingerprinting").
//

import SwiftUI

struct PrinterDetailView: View {

    @Environment(PrinterStore.self) private var store
    let printerId: UUID

    @State private var status: PDDeviceStatus?
    @State private var isRefreshing = false
    @State private var identity: PDIdentity?
    @State private var identifyError: String?
    @State private var isIdentifying = false
    @State private var editedName = ""
    @State private var editedWidth: PaperWidth = .mm80

    private var printer: SavedPrinter? { store.printer(withId: printerId) }

    var body: some View {
        Group {
            if let printer {
                content(for: printer)
            } else {
                ContentUnavailableView("Printer removed", systemImage: "printer.dotmatrix")
            }
        }
        .navigationTitle(printer?.displayName ?? "Printer")
        .navigationBarTitleDisplayMode(.inline)
        .task {
            guard let printer else { return }
            editedName = printer.name
            editedWidth = printer.width
            await refresh(printer)
        }
    }

    @ViewBuilder
    private func content(for printer: SavedPrinter) -> some View {
        List {
            Section("Connection") {
                StatusRow(label: "Address", value: printer.endpoint)
                StatusRow(label: "Driver id", value: printer.driverId)
                StatusRow(label: "Profile", value: printer.profileId ?? "generic (unknown device)")
                if let event = store.lastDeviceEvent[printer.driverId] {
                    StatusRow(label: "Last device event", value: event)
                }
            }

            Section {
                TextField("Name", text: $editedName)
                    .onSubmit { commit(printer) }
                Picker("Paper width", selection: $editedWidth) {
                    ForEach(PaperWidth.allCases) { option in
                        Text(option.detailLabel).tag(option)
                    }
                }
                .onChange(of: editedWidth) { commit(printer) }
            } header: {
                Text("Settings")
            }

            statusSection(for: printer)
            identifySection(for: printer)

            Section {
                Button(role: .destructive) {
                    store.remove(printer)
                } label: {
                    Label("Forget this printer", systemImage: "trash")
                }
            }
        }
    }

    // MARK: Status

    @ViewBuilder
    private func statusSection(for printer: SavedPrinter) -> some View {
        Section {
            if let status {
                TriStateRow(label: "Connected",
                            value: NSNumber(value: status.connected))
                TriStateRow(label: "Online", value: status.online)
                TriStateRow(label: "Paper out", value: status.paperOut, trueIsGood: false)
                TriStateRow(label: "Paper near end", value: status.paperNearEnd, trueIsGood: false)
                TriStateRow(label: "Cover open", value: status.coverOpen, trueIsGood: false)
                TriStateRow(label: "Cutter error", value: status.cutterError, trueIsGood: false)
                TriStateRow(label: "Recoverable error",
                            value: status.recoverableError, trueIsGood: false)
                TriStateRow(label: "Unrecoverable error",
                            value: status.unrecoverableError, trueIsGood: false)
                if !status.observed {
                    Label("Nothing decoded from this device yet.",
                          systemImage: "exclamationmark.triangle")
                        .font(.footnote)
                        .foregroundStyle(.orange)
                }
            } else {
                Text("No snapshot yet.")
                    .foregroundStyle(.secondary)
            }

            Button {
                Task { await refresh(printer) }
            } label: {
                if isRefreshing {
                    HStack { ProgressView(); Text("Querying…") }
                } else {
                    Label("Refresh status", systemImage: "arrow.clockwise")
                }
            }
            .disabled(isRefreshing)
        } header: {
            Text("Status snapshot")
        } footer: {
            Text("DLE EOT 1–4, queued behind any active job. "
                 + "\"not observed\" means the device has not answered that flag — it is "
                 + "not the same as no.")
        }
    }

    // MARK: Identify

    @ViewBuilder
    private func identifySection(for printer: SavedPrinter) -> some View {
        Section {
            Button {
                Task { await identify(printer) }
            } label: {
                if isIdentifying {
                    HStack { ProgressView(); Text("Interrogating…") }
                } else {
                    Label("Identify", systemImage: "sparkle.magnifyingglass")
                }
            }
            .disabled(isIdentifying)

            if let identifyError {
                Text(identifyError)
                    .font(.footnote)
                    .foregroundStyle(.red)
            }

            if let identity {
                StatusRow(label: "Vendor guess", value: identity.vendorGuess)
                StatusRow(label: "Profile guess", value: identity.profileGuess)
                StatusRow(label: "Confidence", value: "\(identity.confidencePercent) %",
                          tint: confidenceTint(identity.confidencePercent))
                StatusRow(label: "identityTrusted",
                          value: identity.identityTrusted ? "true" : "false",
                          tint: identity.identityTrusted ? .green : .orange)
                if identity.impersonationSuspected {
                    Label("This device reports another vendor's identity.",
                          systemImage: "exclamationmark.triangle.fill")
                        .font(.footnote)
                        .foregroundStyle(.orange)
                }
                StatusRow(label: "Completion fence", value: identity.completionMechanism)
                StatusRow(label: "Effective profile", value: identity.profileName)

                DisclosureGroup("What the device reported") {
                    StatusRow(label: "GS I manufacturer",
                              value: identity.reportedManufacturer.isEmpty
                                  ? "not reported" : identity.reportedManufacturer)
                    StatusRow(label: "GS I model",
                              value: identity.reportedModel.isEmpty
                                  ? "not reported" : identity.reportedModel)
                    StatusRow(label: "Firmware",
                              value: identity.firmware.isEmpty ? "not reported" : identity.firmware)
                    StatusRow(label: "Serial",
                              value: identity.serial.isEmpty ? "not reported" : identity.serial)
                    StatusRow(label: "MAC OUI",
                              value: identity.ouiVendor.isEmpty ? "not supplied" : identity.ouiVendor)
                    TriStateRow(label: "GS ( H marker", value: identity.supportsProcessIdMarker)
                    TriStateRow(label: "GS r 1", value: identity.supportsQueuedPaperStatus)
                    TriStateRow(label: "DLE EOT", value: identity.supportsRealtimeStatus)
                }

                if !identity.signals.isEmpty {
                    DisclosureGroup("Signals behind the guess") {
                        ForEach(identity.signals, id: \.self) { signal in
                            Text("• " + signal)
                                .font(.footnote)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
            }
        } header: {
            Text("Identity")
        } footer: {
            Text("A non-destructive probe: DLE EOT, GS I, one GS ( H marker, GS r 1, ASB. "
                 + "It never cuts and never clears the buffer. "
                 + "identityTrusted is true only when a signal independent of GS I agrees "
                 + "with it, because GS I can be borrowed from another vendor.")
        }
    }

    private func confidenceTint(_ percent: Int) -> Color {
        switch percent {
        case 80...: return .green
        case 40..<80: return .orange
        default: return .secondary
        }
    }

    // MARK: Actions

    private func commit(_ printer: SavedPrinter) {
        var updated = printer
        updated.name = editedName.trimmingCharacters(in: .whitespaces)
        updated.width = editedWidth
        store.update(updated)
    }

    private func refresh(_ printer: SavedPrinter) async {
        isRefreshing = true
        status = await PrinterDriverService.shared.refreshStatus(for: printer)
        await store.refresh(printer)
        isRefreshing = false
    }

    private func identify(_ printer: SavedPrinter) async {
        isIdentifying = true
        identifyError = nil
        do {
            identity = try await PrinterDriverService.shared.identify(host: printer.host,
                                                                      port: printer.port)
        } catch {
            identity = nil
            identifyError = error.localizedDescription
        }
        isIdentifying = false
    }
}
