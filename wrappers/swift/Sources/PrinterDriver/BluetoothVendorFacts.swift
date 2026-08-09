import Foundation

// The vendor Bluetooth table of docs/wire-protocols.md §4, as constants
// (docs/compatibility-brief.md §25).
//
// -- Why a table of four rows is worth its own file ------------------------------------
//
// Because three of the four facts here are *negative*, and negative facts are the ones a
// codebase loses. "Epson's BLE GATT map is not published" survives exactly as long as
// somebody remembers it; the moment it is only in a research document, the next person to
// hold a TM-P20II that advertises `FFE0` will wire it to the FFE1 heuristic, watch a test
// page come out, and ship it. It will work on that unit. It is still a guess, and the day
// it stops working there will be nothing in the tree that says it ever was one.
//
// So the absences are recorded as values, with the reason attached, and the selection core
// in BluetoothGATT.swift contains no vendor entry at all — a printer from any of these
// four vendors reaches a generic profile only because its module genuinely answered a
// generic probe, never because a table said it should.

// MARK: - MFi

/// An MFi / ExternalAccessory protocol string, or the documented reason there isn't one.
///
/// Modelled as an enum rather than `String?` because the two absences are not the same
/// thing. "We have not looked it up" is a gap in this repository; ``vendorGated`` is a
/// property of the vendor, unchanged by any amount of further research, and only fixable
/// by an MFi registration. Collapsing them into `nil` would make the second look like the
/// first and invite somebody to go and find it.
///
/// - Important: There is no `guessed` case and there must never be one. An MFi string is
///   matched exactly by iOS against `UISupportedExternalAccessoryProtocols` and against
///   the accessory's own certification; a plausible-looking invention does not fail
///   loudly, it simply never opens a session, which reads on a support call exactly like a
///   printer that is switched off.
public enum MFiProtocolString: Hashable, Sendable, CustomStringConvertible {
  /// The vendor publishes it, and this is it verbatim.
  case published(String)
  /// Issued only through MFi registration and approval — the vendor does not publish it,
  /// so no value can be recorded here honestly.
  case vendorGated

  /// The string to put in `UISupportedExternalAccessoryProtocols`, or `nil` when there is
  /// none to be had.
  public var value: String? {
    switch self {
    case .published(let string): return string
    case .vendorGated: return nil
    }
  }

  /// Whether a value exists at all.
  public var isPublished: Bool { value != nil }

  public var description: String {
    value ?? "vendor-gated: issued only via MFi registration/approval"
  }
}

// MARK: - BLE stance

/// What is publicly known about a vendor's BLE GATT.
public enum BLEProfileStatus: String, Hashable, Sendable, CaseIterable {
  /// The vendor documents no raw GATT map and the documented path is its own SDK. Epson's
  /// TM-P20II BLE needs the ePOS SDK's dedicated profile; Star's SDK addresses printers by
  /// `BLE:<device>` name and keeps the GATT behind it.
  case sdkRequired
  /// No published map and no SDK path recorded here either — Bixolon's SPP-R310 is
  /// dual-mode BLE plus MFi iAP2 with neither side's wire format public.
  case profileUnknown
  /// The device is reachable through one of §4's generic BLE-UART families, having
  /// actually answered the probe. No vendor in this table carries this value.
  case genericUARTProbe

  /// Whether a raw-socket BLE implementation may be attempted at all.
  ///
  /// `false` for both vendor cases, and that is the flag the selection core honours by
  /// containing no vendor entries: mapping a printer whose GATT is unpublished onto
  /// `FFE1` because the numbers are convenient is a guess wearing a heuristic's clothes.
  public var permitsRawGATT: Bool { self == .genericUARTProbe }
}

// MARK: - The table

/// One row of §4's vendor Bluetooth table.
public struct BluetoothVendorFacts: Hashable, Sendable {
  /// The manufacturer, as §4 names it.
  public let vendor: String
  /// Whether the vendor publishes a raw GATT map. `false` for all four rows of §4.
  public let publishesRawGATT: Bool
  /// What may be attempted over BLE.
  public let bleProfileStatus: BLEProfileStatus
  /// The MFi protocol string, or why there isn't one.
  public let mfiProtocol: MFiProtocolString
  /// §4's own words for this row, kept so the reason travels with the fact.
  public let note: String

  public init(
    vendor: String, publishesRawGATT: Bool, bleProfileStatus: BLEProfileStatus,
    mfiProtocol: MFiProtocolString, note: String
  ) {
    self.vendor = vendor
    self.publishesRawGATT = publishesRawGATT
    self.bleProfileStatus = bleProfileStatus
    self.mfiProtocol = mfiProtocol
    self.note = note
  }

  /// Epson: no public raw GATT, MFi string `com.epson.escpos`.
  public static let epson = BluetoothVendorFacts(
    vendor: "Epson",
    publishesRawGATT: false,
    bleProfileStatus: .sdkRequired,
    mfiProtocol: .published("com.epson.escpos"),
    note: "TM-P20II BLE requires the ePOS SDK's dedicated profile; the GATT map is not published.")

  /// Star: no public raw GATT, MFi string `jp.star-m.starpro`.
  public static let star = BluetoothVendorFacts(
    vendor: "Star",
    publishesRawGATT: false,
    bleProfileStatus: .sdkRequired,
    mfiProtocol: .published("jp.star-m.starpro"),
    note: "The SDK addresses printers by BLE:<device> name; the GATT behind it is hidden.")

  /// Bixolon: no public raw GATT, MFi string `com.bixolon.protocol`.
  public static let bixolon = BluetoothVendorFacts(
    vendor: "Bixolon",
    publishesRawGATT: false,
    bleProfileStatus: .profileUnknown,
    mfiProtocol: .published("com.bixolon.protocol"),
    note: "No public SPP-R310 GATT map; the unit is dual-mode BLE plus MFi iAP2.")

  /// Citizen: no public raw GATT, and **no MFi string at all**.
  ///
  /// The absence is the fact. §4 records Citizen's protocol name as vendor-gated — issued
  /// only through MFi registration and approval — so this row carries ``MFiProtocolString/vendorGated``
  /// and not a plausible `com.citizen.*` invention. Citizen Classic BT stays blocked until
  /// the protocol name and the MFi approval both exist, and until they do there is nothing
  /// to put in an Info.plist that would do anything but fail silently.
  public static let citizen = BluetoothVendorFacts(
    vendor: "Citizen",
    publishesRawGATT: false,
    bleProfileStatus: .profileUnknown,
    mfiProtocol: .vendorGated,
    note: "Blocked pending MFi registration/approval; no protocol name may be guessed.")

  /// The whole of §4's table, in its own order.
  public static let all: [BluetoothVendorFacts] = [.epson, .star, .bixolon, .citizen]

  /// The row for a vendor name, case-insensitively; `nil` for a vendor §4 does not cover,
  /// which is not the same as a vendor with nothing to declare.
  public static func facts(vendor name: String) -> BluetoothVendorFacts? {
    all.first { $0.vendor.caseInsensitiveCompare(name) == .orderedSame }
  }
}

extension BluetoothCapabilities {
  /// The §4 vendor table, reachable from the capability type an application already holds.
  ///
  /// A convenience and not a mapping: ``BluetoothCapabilities`` describes one printer's
  /// stacks, this describes what its manufacturer publishes, and no code derives either
  /// from the other.
  public static var vendorFacts: [BluetoothVendorFacts] { BluetoothVendorFacts.all }
}
