import CPrinterDriver
import Foundation
import XCTest

@testable import PrinterDriver

/// The custom-transport boundary, end to end (docs/compatibility-brief.md §25).
///
/// What matters here is not that bytes move. It is that a printer reached over a link the
/// core knows nothing about gets the identical treatment a TCP printer gets — the same
/// preflight, the same ordered fence, the same correlated `GS ( H` token, the same grade
/// and authority in the result. The moment a transport could weaken a completion
/// guarantee, every wrapper would become part of the completion story and the honesty
/// rules would only be as good as the least careful one. So the assertions below are
/// deliberately the same assertions `PrintJobTests` makes about a scripted TCP-like
/// device.
///
/// The transport is written in Swift on purpose. The C test in `capi/tests/test_capi.c`
/// already proves the ABI; this proves the Swift binding of it — the trampoline's
/// retention, the `@convention(c)` thunks, the `Unmanaged` context and the threading.
final class BluetoothTransportTests: XCTestCase {

  // MARK: - A scripted printer on the far side of a Swift transport

  /// A printer that answers the three things the engine asks over the wire, delivered
  /// from its own queue.
  ///
  /// The queue is not a convenience: pd.h forbids feeding bytes from inside `write`,
  /// because that runs on the core's worker thread — the very thread that would have to
  /// service the delivery. Every real Bluetooth stack answers on a different thread
  /// (a CoreBluetooth delegate queue, an ExternalAccessory stream callback, an Android
  /// reader), so a double that answered inline would be testing the core under a
  /// threading model no printer actually has.
  final class ScriptedBluetoothPrinter: BluetoothTransport, @unchecked Sendable {
    let endpointDescription: String

    private let queue = DispatchQueue(label: "test.scripted-bluetooth")
    private let lock = NSLock()
    private var link: BluetoothLink?
    private var received: [UInt8] = []

    private(set) var opens = 0
    private(set) var closes = 0
    private(set) var bytesWritten = 0

    /// Refuses to open, the way an unpaired or out-of-range printer does.
    var refusesToOpen = false
    /// Answers the `GS ( H` process-ID marker. False models a device that takes the
    /// command and never echoes.
    var answersProcessID = true

    init(endpointDescription: String = "bt-spp:00:11:22:33:44:55") {
      self.endpointDescription = endpointDescription
    }

    func open(link: BluetoothLink) throws {
      lock.lock()
      opens += 1
      self.link = link
      lock.unlock()
      if refusesToOpen {
        throw PrinterDriverError("scripted printer refuses to pair")
      }
    }

    func write(_ bytes: UnsafeRawBufferPointer) throws -> Int {
      lock.lock()
      bytesWritten += bytes.count
      received.append(contentsOf: bytes)
      let link = self.link
      lock.unlock()

      let answers = replies(to: Array(bytes))
      if !answers.isEmpty, let link {
        queue.async { link.deliver(answers) }
      }
      return bytes.count
    }

    func close() {
      lock.lock()
      closes += 1
      lock.unlock()
    }

    /// Whether the printer's byte stream contains `needle`.
    func receivedContains(_ needle: String) -> Bool {
      lock.lock()
      defer { lock.unlock() }
      let haystack = received
      let pattern = Array(needle.utf8)
      guard !pattern.isEmpty, haystack.count >= pattern.count else { return false }
      for start in 0...(haystack.count - pattern.count)
      where Array(haystack[start..<(start + pattern.count)]) == pattern {
        return true
      }
      return false
    }

    /// The three answers a healthy ESC/POS printer gives. Scanned rather than parsed:
    /// this is a test double, and the commands it looks for are fixed byte sequences the
    /// engine emits verbatim.
    private func replies(to chunk: [UInt8]) -> [UInt8] {
      var out: [UInt8] = []
      var i = 0
      while i < chunk.count {
        // DLE EOT n — the real-time status the strict preflight asks four of.
        if chunk[i] == 0x10, i + 2 < chunk.count, chunk[i + 1] == 0x04 {
          // Healthy in every register: online, cover closed, paper present, no cutter
          // fault. Bit 4 set and bit 1 set is what the core's parser recognises as a
          // real-time byte at all.
          out.append(chunk[i + 2] == 1 ? 0x16 : 0x12)
          i += 3
          continue
        }
        // GS ( H fn 48 — the process-ID marker: 1D 28 48 06 00 30 30 followed by four
        // printable bytes. The echo is 37 22 <token> 00.
        if chunk[i] == 0x1D, i + 10 < chunk.count, chunk[i + 1] == 0x28, chunk[i + 2] == 0x48,
          chunk[i + 5] == 0x30, chunk[i + 6] == 0x30
        {
          if answersProcessID {
            out.append(contentsOf: [0x37, 0x22])
            out.append(contentsOf: chunk[(i + 7)...(i + 10)])
            out.append(0x00)
          }
          i += 11
          continue
        }
        // GS r 1 — the queued paper status, answered only once the data ahead of it has
        // printed, which is what makes it a fence rather than a poll.
        if chunk[i] == 0x1D, i + 2 < chunk.count, chunk[i + 1] == 0x72 {
          out.append(0x00)
          i += 3
          continue
        }
        i += 1
      }
      return out
    }
  }

  private let receipt = Payload.text(["BLUETOOTH TICKET", "Order 7F3A", "1x Soup"])

  // MARK: - The point of the whole boundary

  func testABluetoothJobEarnsExactlyTheGradeATcpJobWould() throws {
    let driver = try PrinterDriver(fsyncDisabled: true)
    let device = ScriptedBluetoothPrinter()
    // 384 dots and a 58 mm portable profile: the deployed shape for a handheld, and a
    // reminder that the 576 default is a desktop assumption.
    let printer = try driver.printer(bluetooth: device, width: .dots384, profile: "xp-s260m")

    // The id comes from the transport's own description, so two paired printers are
    // distinguishable without the caller inventing names for them.
    XCTAssertTrue(printer.id.contains("bt-spp"))
    XCTAssertEqual(printer.completionMechanism, .gsParenH)

    let job = try printer.print(receipt, options: JobOptions(key: "bt-order-1"))
    runAsync(in: self) {
      switch await job.result {
      case .done(let confidence, let grade, let authority, let method):
        XCTAssertEqual(confidence, .cutFaultFree)
        XCTAssertEqual(grade, .aJobLevelConfirmation)
        XCTAssertEqual(grade.letter, "A")
        XCTAssertEqual(authority, .physicalPrinter)
        XCTAssertEqual(method, "GS(H) fn48")
      case .failed(let reason, _):
        XCTFail("expected done, got failed(\(reason))")
      case .unknown:
        XCTFail("expected done, got unknown")
      }
    }

    XCTAssertGreaterThanOrEqual(device.opens, 1)
    XCTAssertGreaterThan(device.bytesWritten, 0)
    XCTAssertTrue(device.receivedContains("BLUETOOTH TICKET"))
    // The receipt carries a verification identifier, so a printed ticket resolves back to
    // this job — over Bluetooth exactly as over TCP.
    let token = try XCTUnwrap(job.printToken)
    XCTAssertFalse(token.isEmpty)
    XCTAssertNotNil(driver.job(token: token))
  }

  func testAPrinterThatNeverEchoesLeavesTheJobUnknownRatherThanDone() throws {
    // The honest failure. Bytes went out and the receipt may well be in somebody's hand;
    // what did not happen is the acknowledgement. Reporting that as success is the bug
    // that produces duplicate tickets, and the transport being Bluetooth changes nothing.
    let driver = try PrinterDriver(fsyncDisabled: true)
    let device = ScriptedBluetoothPrinter()
    device.answersProcessID = false
    let printer = try driver.printer(
      bluetooth: device, width: .dots384, profile: "xp-s260m")

    let job = try printer.print(
      receipt, options: JobOptions(key: "bt-silent-1", timeoutMilliseconds: 900))
    runAsync(in: self) {
      switch await job.result {
      case .done:
        XCTFail("a device that never acknowledged must not report done")
      case .failed(let reason, _):
        XCTFail("expected unknown, got failed(\(reason))")
      case .unknown(let confidence):
        // Not zero: preflight passed and the bytes were accepted, which is what an
        // operator needs in order to decide about a reprint.
        XCTAssertGreaterThanOrEqual(confidence.rawValue, ConfidenceLevel.transportAccepted.rawValue)
      }
    }
    XCTAssertGreaterThan(device.bytesWritten, 0)
  }

  func testAnUnpairedPrinterFailsAsUnreachableWithoutWritingAnything() throws {
    let driver = try PrinterDriver(fsyncDisabled: true)
    let device = ScriptedBluetoothPrinter()
    device.refusesToOpen = true
    let printer = try driver.printer(bluetooth: device, width: .dots384)

    let job = try printer.print(receipt, options: JobOptions(key: "bt-unpaired-1"))
    runAsync(in: self) {
      switch await job.result {
      case .failed(let reason, _):
        XCTAssertEqual(reason, .transportUnreachable)
      case .done, .unknown:
        XCTFail("a link that never opened cannot have printed")
      }
    }
    XCTAssertEqual(device.bytesWritten, 0)
  }

  func testZebraAndBrotherAreRefusedBeforeTheLinkIsEvenOpened() throws {
    // docs/compatibility-brief.md §16 and §17. ZPL, CPCL and Brother Raster are not
    // ESC/POS at any level, and an ESC/POS engine pointed at one prints a metre of text
    // instead of a label. The refusal has to happen before a byte is written — a refusal
    // that still wasted a roll would be worse than the bug it replaced.
    for profile in ["zebra_zq300_plus", "zebra_zq600_plus", "brother_rj2000", "brother_rj4000"] {
      let driver = try PrinterDriver(fsyncDisabled: true)
      let device = ScriptedBluetoothPrinter(endpointDescription: "bt-spp:\(profile)")
      let printer = try driver.printer(bluetooth: device, width: .dots384, profile: profile)

      let job = try printer.print(receipt, options: JobOptions(key: "label-\(profile)"))
      runAsync(in: self) {
        switch await job.result {
        case .failed(let reason, let confidence):
          XCTAssertEqual(reason, .unsupported, profile)
          XCTAssertEqual(confidence, .transportAccepted, profile)
        case .done, .unknown:
          XCTFail("\(profile) is not an ESC/POS printer and must be refused")
        }
      }
      XCTAssertEqual(device.bytesWritten, 0, profile)
      XCTAssertEqual(device.opens, 0, profile)
    }
  }

  func testAnUnknownProfileIsAnErrorRatherThanASilentDowngrade() throws {
    // A caller that asked for a TM-T88VI and quietly got an unknown-device profile would
    // be told a weaker completion story than it asked for, with nothing in the result
    // explaining why.
    let driver = try PrinterDriver(fsyncDisabled: true)
    let device = ScriptedBluetoothPrinter()
    XCTAssertThrowsError(try driver.printer(bluetooth: device, profile: "epson_tm_t99"))
    XCTAssertEqual(device.opens, 0)
  }

  func testTheCatalogueIsEnumerableAndCarriesTheNewFamilies() throws {
    // docs/compatibility-brief.md §26. Wrappers enumerate `pd_profile_ids` instead of
    // hardcoding names, so an id missing here is a printer nobody can select.
    let ids = PrinterDriver.profileIDs
    XCTAssertGreaterThanOrEqual(ids.count, 80)
    for expected in [
      "generic", "xp-s260m", "epson_tm_t20iii", "epson_tm_t88vi", "epson_tm_p20ii",
      "epson_tm_p80ii_autocutter", "star_sm_s230", "bixolon_spp_r310", "citizen_cmp_30ii",
      "rongta_rp8xx", "xprinter_v_series", "partner_rp710", "zebra_zq600_plus",
      "brother_rj4000", "generic_unknown",
    ] {
      XCTAssertTrue(ids.contains(expected), "missing profile id \(expected)")
    }
  }

  func testProvenanceDistinguishesDocumentedFromAdvertisedBeforeAnythingIsPrinted() throws {
    // docs/compatibility-brief.md §28. This is the distinction the whole brief turns on,
    // and its value is that it is available up front: an Epson whose fence is in the
    // manufacturer's command table is a different proposition from a clone whose fence is
    // a hopeful default, and a job result cannot tell you which you have because both
    // report the same grade when they work.
    let driver = try PrinterDriver(fsyncDisabled: true)
    let cases: [(profile: String, provenance: Provenance, language: CommandLanguage)] = [
      ("epson_tm_t88vi", .documented, .escPos),
      ("epson_tm_p20ii", .documented, .escPos),
      // Advertised, never documented: §14 Xprinter, §13 Rongta, §15 Partner.
      ("xprinter_s_series", .unverified, .escPos),
      ("rongta_rp80", .unverified, .escPos),
      ("partner_rp110", .unverified, .escPos),
      // The one interrogated unit in the database.
      ("xp-s260m", .probed, .escPos),
      // Not ESC/POS at any level, and the language says so before anything is sent.
      ("zebra_zq600_plus", .unverified, .zpl),
      ("brother_rj4000", .unverified, .brotherRaster),
    ]
    for testCase in cases {
      let printer = try driver.printer(
        bluetooth: ScriptedBluetoothPrinter(endpointDescription: "bt-spp:\(testCase.profile)"),
        width: .dots384,
        profile: testCase.profile)
      XCTAssertEqual(printer.completionProvenance, testCase.provenance, testCase.profile)
      XCTAssertEqual(printer.language, testCase.language, testCase.profile)
    }
  }

  // MARK: - Capability facets

  func testBluetoothCapabilitiesAreFacetedRatherThanOneBoolean() {
    // docs/compatibility-brief.md §25. Five different stacks hide behind
    // "bluetooth: true", and picking the wrong one is a printer that never connects.
    let epson = BluetoothCapabilities.epsonPortable
    XCTAssertTrue(epson.classicSPP)  // TM-P20II-xxxxxx
    XCTAssertTrue(epson.ble)  // TM-P20II-xxxxxx-L
    XCTAssertFalse(epson.mfi)
    XCTAssertFalse(epson.vendorSDK)
    XCTAssertTrue(epson.any)
    // §6: 4 KB normally, 64 KB over Bluetooth. Same printer, two documented numbers,
    // neither derivable from the other.
    XCTAssertEqual(epson.receiveBufferBytes, 4 * 1024)
    XCTAssertEqual(epson.bluetoothReceiveBufferBytes, 64 * 1024)

    // A Star portable is driven through Star's SDK, not a raw socket; a Citizen CMP sells
    // an MFi variant, which is a different iOS code path from BLE. Neither is expressible
    // as "has Bluetooth".
    let star = BluetoothCapabilities(classicVendor: true, ble: true, vendorSDK: true)
    XCTAssertFalse(star.classicSPP)
    XCTAssertTrue(star.vendorSDK)
    let citizen = BluetoothCapabilities(classicSPP: true, mfi: true)
    XCTAssertTrue(citizen.mfi)
    XCTAssertFalse(citizen.ble)

    XCTAssertFalse(BluetoothCapabilities().any)
  }
}
