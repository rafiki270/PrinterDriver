import CPrinterDriver
import Foundation

// The Bluetooth boundary (docs/compatibility-brief.md §25).
//
// Bluetooth does not live in the core and should not. On Apple it is CoreBluetooth or
// ExternalAccessory, each a framework with its own permissions model, its own pairing UI
// and its own threading; linking either into a portable C++17 core would make the core
// unportable in order to buy one transport.
//
// So the split is: **the platform owns the socket, the core owns the protocol.** An
// implementation of ``BluetoothTransport`` answers exactly one question — can bytes
// move? — and everything that makes this SDK worth using stays on the other side of the
// boundary: the ordered fence, the `GS ( H` correlation token, preflight, the journal,
// the confidence grading, the refusal to overclaim. A job over Bluetooth reports the
// same grade a job over TCP reports, because the transport is not the thing making the
// claim. That is the property this design exists to protect: the moment a transport
// could weaken a completion guarantee, every wrapper would become part of the
// completion story, and the honesty rules would only be as good as the least careful
// one.

// MARK: - Link

/// The core's side of a custom transport: how an implementation pushes received bytes
/// back in, and how it reports that the link died.
///
/// Handed to ``BluetoothTransport/open(link:)`` and valid until the driver is destroyed.
/// It survives reconnection — the core builds a fresh internal transport after a link
/// drop, and an implementation keeps using the link it was given.
public struct BluetoothLink: @unchecked Sendable {
  private let printer: OpaquePointer

  init(printer: OpaquePointer) { self.printer = printer }

  /// Delivers bytes the link received.
  ///
  /// - Returns: `true` when the bytes reached the core's response parser; `false` when
  ///   nothing was listening, because the core has not connected yet or has already
  ///   closed. `false` is information rather than an error: bytes arriving with no reader
  ///   are dropped, exactly as they would be on a socket nobody is reading. A BLE
  ///   notification already in flight when the core closes is the normal way to see it.
  ///
  /// - Important: Callable from **any** thread — a CoreBluetooth delegate queue, an
  ///   ExternalAccessory stream callback, your own reader — including while
  ///   ``BluetoothTransport/write(_:)`` is in flight, which is the usual case: a status
  ///   answer comes back while the next chunk goes out. It must **not** be called from
  ///   inside `open`, `write` or `close`: those run on the core's worker thread, which is
  ///   the thread that would have to service the delivery.
  @discardableResult
  public func deliver(_ bytes: UnsafeRawBufferPointer) -> Bool {
    guard let base = bytes.baseAddress, !bytes.isEmpty else { return false }
    return pd_transport_feed_bytes(
      printer, base.assumingMemoryBound(to: UInt8.self), bytes.count) != 0
  }

  /// Delivers bytes the link received.
  @discardableResult
  public func deliver(_ bytes: [UInt8]) -> Bool {
    bytes.withUnsafeBytes { deliver($0) }
  }

  /// Delivers bytes the link received.
  @discardableResult
  public func deliver(_ data: Data) -> Bool {
    data.withUnsafeBytes { deliver($0) }
  }

  /// Reports that the link dropped for a reason other than an explicit ``BluetoothTransport/close()``
  /// — the peer went out of range, the OS tore the channel down, pairing was revoked.
  ///
  /// Surfaces as ``DeviceEvent/connectionLost`` and fails any job waiting on a fence
  /// straight away, instead of leaving it to burn through its completion timeout while an
  /// operator watches a spinner.
  ///
  /// - Returns: `true` when a live transport was notified.
  @discardableResult
  public func linkDropped(_ message: String) -> Bool {
    message.withCString { pd_transport_link_dropped(printer, $0) != 0 }
  }
}

// MARK: - Transport

/// A Bluetooth link the application owns.
///
/// Implement this over CoreBluetooth, ExternalAccessory or a vendor SDK and hand it to
/// ``PrinterDriver/printer(bluetooth:width:profile:)``. See ``CoreBluetoothTransport`` and
/// ``ExternalAccessoryTransport`` for skeletons.
///
/// ### Threading
///
/// `open`, `write` and `close` are invoked on the core's worker thread for this printer,
/// one at a time and never concurrently with each other. They may block — the worker is
/// the thread that would otherwise be waiting anyway — but a `write` that blocks for
/// longer than the profile's completion timeout will simply run the job out of budget.
///
/// Received bytes go back through ``BluetoothLink``, from whatever thread the platform
/// hands them to you on, and never from inside these three methods.
///
/// ### What an implementation must not do
///
/// It must not call back into ``PrinterDriver``, ``Printer`` or ``PrintJob`` on the same
/// driver: the worker thread it is running on is the thread that would have to service
/// that call. Copy what you need and signal your own loop.
public protocol BluetoothTransport: AnyObject {
  /// Opens the link, and keeps `link` for the lifetime of the transport.
  ///
  /// - Throws: anything at all; the job fails with ``FailureReason/transportUnreachable``
  ///   and the thrown error is not otherwise inspected.
  func open(link: BluetoothLink) throws

  /// Hands `bytes` to the link.
  ///
  /// - Returns: the number of bytes actually transferred. A short return is reported
  ///   honestly and is not an error to be rounded away: zero bytes out is a known failure
  ///   and one byte out leaves the job ``JobResult/unknown(confidence:)``, and that
  ///   difference is what tells an operator whether reprinting risks a duplicate.
  /// - Throws: for a hard failure, which is treated as zero bytes transferred.
  func write(_ bytes: UnsafeRawBufferPointer) throws -> Int

  /// Closes the link. Called once per successful ``open(link:)``, and again is harmless.
  ///
  /// The driver closes on teardown, which matters more on Bluetooth than on TCP: the
  /// Epson portables document a **single simultaneous connection** and a limited number
  /// of stored pairings, so a link left open is a printer nobody else can reach.
  func close()

  /// What the printer id and the diagnostics derive from, e.g.
  /// `"bt-spp:00:11:22:33:44:55"`. Two printers with the same description are two
  /// printers with the same id.
  var endpointDescription: String { get }
}

// MARK: - Capabilities

/// Which Bluetooth path a device actually offers — the `BluetoothTransport` record of
/// docs/compatibility-brief.md §25.
///
/// **Never `bluetooth = true`.** Five different things hide behind that boolean and they
/// need five different stacks, so a single flag is not a simplification but a way to pick
/// the wrong one. An Epson TM-P20II documents Classic *and* BLE, advertising as
/// `TM-P20II-xxxxxx` and `TM-P20II-xxxxxx-L`; a Star SM-S230i is driven through Star's
/// SDK; Citizen sells an MFi variant of the CMP; Zebra exposes separate Classic and BLE
/// status connections through Link-OS.
public struct BluetoothCapabilities: Hashable, Sendable {
  /// Bluetooth Classic, Serial Port Profile: an ordinary byte stream.
  public var classicSPP: Bool
  /// Classic, but framed by the vendor's own protocol rather than bare SPP.
  public var classicVendor: Bool
  /// Bluetooth Low Energy, GATT.
  public var ble: Bool
  /// Apple MFi / ExternalAccessory — a different iOS code path from BLE, and one that
  /// cannot be inferred from either of the others.
  public var mfi: Bool
  /// The documented path is the vendor's SDK, not a raw socket.
  public var vendorSDK: Bool

  /// The receive buffer the manufacturer documents for the non-Bluetooth path, in bytes.
  /// `nil` when undocumented, which is not the same as "small".
  public var receiveBufferBytes: Int?
  /// The receive buffer documented **for Bluetooth**, in bytes.
  ///
  /// Recorded separately because it is genuinely a different number on the same printer:
  /// the Epson portables document 4 KB normally and **64 KB over Bluetooth**
  /// (docs/compatibility-brief.md §6). Pacing derived from a model name rather than from
  /// the path in use gets this wrong in whichever direction hurts.
  public var bluetoothReceiveBufferBytes: Int?

  public init(
    classicSPP: Bool = false,
    classicVendor: Bool = false,
    ble: Bool = false,
    mfi: Bool = false,
    vendorSDK: Bool = false,
    receiveBufferBytes: Int? = nil,
    bluetoothReceiveBufferBytes: Int? = nil
  ) {
    self.classicSPP = classicSPP
    self.classicVendor = classicVendor
    self.ble = ble
    self.mfi = mfi
    self.vendorSDK = vendorSDK
    self.receiveBufferBytes = receiveBufferBytes
    self.bluetoothReceiveBufferBytes = bluetoothReceiveBufferBytes
  }

  /// Whether this device offers Bluetooth at all.
  public var any: Bool { classicSPP || classicVendor || ble || mfi || vendorSDK }

  /// The Epson portable facts of docs/compatibility-brief.md §6: Bluetooth 5.0 Dual Mode
  /// — Classic and LE — with a 4 KB receive buffer normally and 64 KB over Bluetooth.
  /// Both numbers are documented; neither is derivable from the other.
  public static let epsonPortable = BluetoothCapabilities(
    classicSPP: true,
    ble: true,
    receiveBufferBytes: 4 * 1024,
    bluetoothReceiveBufferBytes: 64 * 1024)
}

// MARK: - Registration

/// Holds a ``BluetoothTransport`` alive for as long as the driver can call into it, and
/// carries the C function pointers' context.
///
/// `pd.h` has no unsubscribe: a context handed to the ABI must stay valid until
/// `pd_destroy`. `DriverCore.retain(trampoline:)` is the existing mechanism for exactly
/// that, and this joins the same list.
final class BluetoothTrampoline {
  let transport: BluetoothTransport
  /// Set immediately after `pd_add_printer_custom` returns, before the core can connect:
  /// the printer handle is what a link delivers to, and it does not exist until then.
  var link: BluetoothLink?

  init(transport: BluetoothTransport) { self.transport = transport }
}

private func bluetoothConnect(_ ctx: UnsafeMutableRawPointer?) -> Int32 {
  guard let ctx else { return 0 }
  let trampoline = Unmanaged<BluetoothTrampoline>.fromOpaque(ctx).takeUnretainedValue()
  guard let link = trampoline.link else { return 0 }
  do {
    try trampoline.transport.open(link: link)
    return 1
  } catch {
    return 0
  }
}

private func bluetoothWrite(
  _ ctx: UnsafeMutableRawPointer?, _ data: UnsafePointer<UInt8>?, _ size: Int
) -> Int64 {
  guard let ctx, let data else { return -1 }
  let trampoline = Unmanaged<BluetoothTrampoline>.fromOpaque(ctx).takeUnretainedValue()
  do {
    let written = try trampoline.transport.write(
      UnsafeRawBufferPointer(start: UnsafeRawPointer(data), count: size))
    return Int64(written)
  } catch {
    return -1
  }
}

private func bluetoothClose(_ ctx: UnsafeMutableRawPointer?) {
  guard let ctx else { return }
  Unmanaged<BluetoothTrampoline>.fromOpaque(ctx).takeUnretainedValue().transport.close()
}

extension PrinterDriver {
  /// Configures a printer reached over a link the application owns, and starts its worker.
  ///
  /// Everything the SDK does about completion, correlation and honesty happens above this
  /// boundary, so a Bluetooth printer behaves exactly like a TCP one and reports exactly
  /// the same grades — see the note at the top of `BluetoothTransport.swift`.
  ///
  /// - Parameters:
  ///   - transport: the link. Retained by the driver until the driver is destroyed.
  ///   - width: printable width in dots. Note that portable printers are usually 384
  ///     (58 mm paper, 48 mm image), not the 576 default.
  ///   - profile: a capability-profile id from ``profileIDs``, e.g. `"epson_tm_p20ii"`.
  ///     `nil` means `"generic"`, which claims nothing beyond the queued fence. Passing
  ///     an id the core does not know is an error rather than a silent downgrade.
  /// - Returns: a handle owned by this driver.
  public func printer(
    bluetooth transport: BluetoothTransport,
    width: PrinterWidth = .dots384,
    profile: String? = nil
  ) throws -> Printer {
    let trampoline = BluetoothTrampoline(transport: transport)
    // Retained before the ABI ever sees the pointer, and never released early: the core
    // may call connect on its worker thread the moment the first job arrives.
    core.retain(trampoline: trampoline)
    let ctx = Unmanaged.passUnretained(trampoline).toOpaque()

    let handle = try transport.endpointDescription.withCString { description -> OpaquePointer in
      try withOptionalCString(profile) { profilePointer in
        var vtable = pd_transport_vtable(
          connect: bluetoothConnect,
          write: bluetoothWrite,
          close: bluetoothClose,
          description: description)
        guard
          let handle = withUnsafePointer(to: &vtable, {
            pd_add_printer_custom(core.handle, $0, ctx, profilePointer, width.rawValue)
          })
        else {
          throw core.lastError()
        }
        return handle
      }
    }
    trampoline.link = BluetoothLink(printer: handle)
    return Printer(core: core, handle: handle)
  }
}
