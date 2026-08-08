//
//  SavedPrinter.swift
//  ReceiptStudio
//

import Foundation

/// A printer the operator chose to keep. Config is transport + width + optional
/// profile hint, matching PrinterConfig in docs/api.md §2.
struct SavedPrinter: Codable, Identifiable, Hashable, Sendable {
    var id: UUID
    var name: String
    var host: String
    var port: UInt16
    var width: PaperWidth
    /// Device-database entry name, e.g. "epson_tm_t20". Nil means "unknown device":
    /// the core uses the conservative generic profile and promotes it only if a
    /// probe establishes something (docs/capability-profiles.md §8).
    var profileId: String?
    var addedAt: Date

    init(id: UUID = UUID(),
         name: String = "",
         host: String,
         port: UInt16 = 9100,
         width: PaperWidth = .mm80,
         profileId: String? = nil,
         addedAt: Date = Date()) {
        self.id = id
        self.name = name
        self.host = host
        self.port = port
        self.width = width
        self.profileId = profileId
        self.addedAt = addedAt
    }

    var displayName: String { name.isEmpty ? host : name }

    var endpoint: String { port == 9100 ? host : "\(host):\(port)" }

    /// The id the bridge derives from the transport description.
    var driverId: String { "\(host):\(port)" }
}

/// A candidate found by the scan, not yet saved.
struct DiscoveredPrinter: Identifiable, Hashable, Sendable {
    var id: String { host }
    let host: String
    let port: UInt16
    let respondedIn: TimeInterval
}
