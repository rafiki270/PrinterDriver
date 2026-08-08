//
//  PrintersView.swift
//  ReceiptStudio
//

import SwiftUI

struct PrintersView: View {

    @Environment(PrinterStore.self) private var store
    @State private var scanner = LanScanner()
    @State private var showingAdd = false

    var body: some View {
        NavigationStack {
            List {
                savedSection
                scanSection
            }
            .listStyle(.insetGrouped)
            .navigationTitle("Printers")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        showingAdd = true
                    } label: {
                        Label("Add printer", systemImage: "plus")
                    }
                }
                ToolbarItem(placement: .topBarLeading) {
                    if scanner.isScanning {
                        Button("Stop") { scanner.cancel() }
                    } else {
                        Button {
                            scanner.start()
                        } label: {
                            Label("Scan", systemImage: "dot.radiowaves.left.and.right")
                        }
                    }
                }
            }
            .sheet(isPresented: $showingAdd) {
                AddPrinterView()
            }
            .refreshable {
                await store.refreshAll()
            }
        }
    }

    // MARK: Saved

    @ViewBuilder
    private var savedSection: some View {
        Section {
            if store.printers.isEmpty {
                ContentUnavailableView {
                    Label("No printers yet", systemImage: "printer")
                } description: {
                    Text("Scan the local network, or add one by IP address.")
                }
                .listRowBackground(Color.clear)
            } else {
                ForEach(store.printers) { printer in
                    NavigationLink {
                        PrinterDetailView(printerId: printer.id)
                    } label: {
                        SavedPrinterRow(printer: printer, liveness: store.liveness(for: printer))
                    }
                }
                .onDelete { store.remove(at: $0) }
            }
        } header: {
            Text("Saved")
        } footer: {
            if !store.printers.isEmpty {
                Text("The dot is green only when a status answer was actually decoded. "
                     + "A printer that has not answered shows as unknown, not as healthy.")
            }
        }
    }

    // MARK: Scan

    @ViewBuilder
    private var scanSection: some View {
        Section {
            if scanner.isScanning {
                VStack(alignment: .leading, spacing: 8) {
                    ProgressView(value: scanner.progress.fraction)
                    HStack {
                        Text(scanner.subnet ?? "")
                            .font(.footnote.monospaced())
                        Spacer()
                        Text("\(scanner.progress.scanned)/\(scanner.progress.total)")
                            .font(.footnote.monospacedDigit())
                    }
                    .foregroundStyle(.secondary)
                }
                .padding(.vertical, 4)
            }

            if let message = scanner.message, !scanner.isScanning {
                Text(message)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }

            ForEach(scanner.found) { candidate in
                DiscoveredRow(candidate: candidate,
                              alreadySaved: store.contains(host: candidate.host,
                                                           port: candidate.port)) {
                    store.add(SavedPrinter(name: "",
                                           host: candidate.host,
                                           port: candidate.port,
                                           width: .mm80))
                }
            }
        } header: {
            Text("Found on the network")
        } footer: {
            Text("An open port 9100 is a candidate, not a printer. "
                 + "Save it, then use Identify to ask the device what it is.")
        }
    }
}

private struct SavedPrinterRow: View {
    let printer: SavedPrinter
    let liveness: Liveness

    var body: some View {
        HStack(spacing: 12) {
            LivenessDot(liveness: liveness)
            VStack(alignment: .leading, spacing: 2) {
                Text(printer.displayName)
                    .font(.body)
                HStack(spacing: 6) {
                    Text(printer.endpoint)
                        .monospaced()
                    Text("·")
                    Text(printer.width.label)
                    if case .attention(let reason) = liveness {
                        Text("·")
                        Text(reason).foregroundStyle(.orange)
                    }
                }
                .font(.caption)
                .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 2)
    }
}

private struct DiscoveredRow: View {
    let candidate: DiscoveredPrinter
    let alreadySaved: Bool
    let save: () -> Void

    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(candidate.host)
                    .monospaced()
                Text("port \(candidate.port) answered in \(Int(candidate.respondedIn * 1000)) ms")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            if alreadySaved {
                Label("Saved", systemImage: "checkmark")
                    .labelStyle(.iconOnly)
                    .foregroundStyle(.green)
            } else {
                Button("Save", action: save)
                    .buttonStyle(.bordered)
                    .controlSize(.small)
            }
        }
    }
}

// MARK: - Add

struct AddPrinterView: View {

    @Environment(PrinterStore.self) private var store
    @Environment(\.dismiss) private var dismiss

    @State private var name = ""
    @State private var host = ""
    @State private var port = "9100"
    @State private var width: PaperWidth = .mm80
    @State private var profileId: String = ""

    private var isValid: Bool {
        let parts = host.split(separator: ".")
        guard parts.count == 4 else { return false }
        return parts.allSatisfy { Int($0).map { (0...255).contains($0) } ?? false }
            && UInt16(port) != nil
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Address") {
                    TextField("IP address", text: $host)
                        .keyboardType(.numbersAndPunctuation)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                        .monospaced()
                    TextField("Port", text: $port)
                        .keyboardType(.numberPad)
                        .monospaced()
                }

                Section("Details") {
                    TextField("Name (optional)", text: $name)
                    Picker("Paper width", selection: $width) {
                        ForEach(PaperWidth.allCases) { option in
                            Text(option.detailLabel).tag(option)
                        }
                    }
                }

                Section {
                    Picker("Profile", selection: $profileId) {
                        Text("Unknown device (generic)").tag("")
                        ForEach(PrinterDriverService.profileNames, id: \.self) { name in
                            Text(name).tag(name)
                        }
                    }
                } header: {
                    Text("Capability profile")
                } footer: {
                    Text("Leave this on generic unless you know the model. "
                         + "A profile only supplies defaults; the Identify probe overrides "
                         + "them with what the device actually does.")
                }
            }
            .navigationTitle("Add printer")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        store.add(SavedPrinter(name: name.trimmingCharacters(in: .whitespaces),
                                               host: host.trimmingCharacters(in: .whitespaces),
                                               port: UInt16(port) ?? 9100,
                                               width: width,
                                               profileId: profileId.isEmpty ? nil : profileId))
                        dismiss()
                    }
                    .disabled(!isValid)
                }
            }
        }
    }
}
