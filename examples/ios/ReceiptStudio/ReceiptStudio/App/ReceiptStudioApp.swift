//
//  ReceiptStudioApp.swift
//  ReceiptStudio
//
//  The iOS example app for the PrinterDriver SDK: find printers, design a receipt
//  in the document DSL, print it, and see honestly what happened.
//

import SwiftUI

@main
struct ReceiptStudioApp: App {

    @State private var printers = PrinterStore()
    @State private var designer = DesignerStore()

    var body: some Scene {
        WindowGroup {
            RootView()
                .environment(printers)
                .environment(designer)
        }
    }
}

struct RootView: View {

    @Environment(PrinterStore.self) private var printers
    @State private var selection: Tab = .printers

    enum Tab: Hashable {
        case printers, designer
    }

    var body: some View {
        TabView(selection: $selection) {
            PrintersView()
                .tabItem {
                    Label("Printers", systemImage: "printer")
                }
                .tag(Tab.printers)

            DesignerView()
                .tabItem {
                    Label("Designer", systemImage: "doc.text.below.ecg")
                }
                .tag(Tab.designer)
        }
        .task {
            printers.startPolling()
        }
    }
}

// MARK: - Shared chrome

/// The one place the app decides what a colour means, so Unknown can never be
/// painted with the same brush as Done anywhere in the UI.
enum OutcomePalette {
    static func color(for outcome: PDJobOutcome) -> Color {
        switch outcome {
        case .done: return .green
        case .failed: return .red
        case .unknown: return .orange
        @unknown default: return .orange
        }
    }

    static func symbol(for outcome: PDJobOutcome) -> String {
        switch outcome {
        case .done: return "checkmark.seal.fill"
        case .failed: return "xmark.octagon.fill"
        case .unknown: return "questionmark.diamond.fill"
        @unknown default: return "questionmark.diamond.fill"
        }
    }

    static func title(for outcome: PDJobOutcome) -> String {
        switch outcome {
        case .done: return "Done"
        case .failed: return "Failed"
        case .unknown: return "Unknown"
        @unknown default: return "Unknown"
        }
    }
}

struct LivenessDot: View {
    let liveness: Liveness

    var body: some View {
        Circle()
            .fill(color)
            .frame(width: 10, height: 10)
            .overlay {
                if case .checking = liveness {
                    Circle().stroke(Color.secondary.opacity(0.4), lineWidth: 1)
                }
            }
            .accessibilityLabel(label)
    }

    private var color: Color {
        switch liveness {
        case .online: return .green
        case .offline: return .red
        case .attention: return .orange
        case .checking: return .secondary.opacity(0.5)
        case .unknown: return .secondary.opacity(0.3)
        }
    }

    private var label: String {
        switch liveness {
        case .online: return "Online"
        case .offline: return "Offline"
        case .attention(let reason): return reason
        case .checking: return "Checking"
        case .unknown: return "Not heard from"
        }
    }
}

struct StatusRow: View {
    let label: String
    let value: String
    var tint: Color = .primary

    var body: some View {
        HStack {
            Text(label)
                .foregroundStyle(.secondary)
            Spacer(minLength: 12)
            Text(value)
                .foregroundStyle(tint)
                .multilineTextAlignment(.trailing)
        }
        .font(.subheadline)
    }
}

/// A flag that has three answers and shows all three. `nil` is "never observed",
/// which is not the same as "false" and must not be drawn as one.
struct TriStateRow: View {
    let label: String
    let value: NSNumber?
    /// Whether `true` is the bad answer (paper out) or the good one (online).
    var trueIsGood: Bool = true

    var body: some View {
        StatusRow(label: label, value: text, tint: tint)
    }

    private var text: String {
        guard let value else { return "not observed" }
        return value.boolValue ? "yes" : "no"
    }

    private var tint: Color {
        guard let value else { return .secondary }
        let good = value.boolValue == trueIsGood
        return good ? .green : .orange
    }
}
