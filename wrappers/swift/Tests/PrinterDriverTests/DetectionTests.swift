import CPrinterDriver
import CPrinterDriverTestSupport
import Foundation
import XCTest

@testable import PrinterDriver

/// M15 — self-test, auto-detection and LAN discovery through the Swift wrapper
/// (docs/api.md §15).
///
/// The wrapper contains no detection logic, so what is under test here is that none of it
/// is *lost* on the way across: that a Done at grade A is still a Done at grade A, that a
/// printless probe's refusal to promote a provenance survives the bridge, and that a
/// sweep which must not print still does not print.

/// A thread-safe sink for the callbacks the sweep fires from its workers.
private final class DeviceCollector: @unchecked Sendable {
  private let lock = NSLock()
  private var devices: [DiscoveredDevice] = []

  func append(_ device: DiscoveredDevice) {
    lock.lock()
    devices.append(device)
    lock.unlock()
  }

  var count: Int {
    lock.lock()
    defer { lock.unlock() }
    return devices.count
  }
}

/// M15 — self-test, auto-detection and LAN discovery through the Swift wrapper
/// (docs/api.md §15).
final class DetectionTests: XCTestCase {

  func testSelfTestPrintsOneTicketAndReportsWhatItProved() throws {
    let driver = try PrinterDriver(fsyncDisabled: true)
    let bench = try driver.scriptedPrinter(id: "bench", .healthy)

    let result: SelfTestResult = try bench.selfTest()

    // The proof is the ordinary tri-state result of the ordinary engine.
    guard case .done(let confidence, let grade, let authority, let method) = result.result
    else {
      return XCTFail("expected a Done self-test, got \(result.result)")
    }
    XCTAssertEqual(confidence, .cutFaultFree)
    XCTAssertEqual(grade, .aJobLevelConfirmation)
    XCTAssertEqual(authority, .physicalPrinter)
    XCTAssertEqual(method, "GS(H) fn48")

    XCTAssertTrue(result.key.hasPrefix("selftest-"))
    XCTAssertEqual(result.verificationID?.count, 4)
    XCTAssertNotNil(result.job)
    XCTAssertEqual(result.job?.key, result.key)

    // The ticket, and the bytes the device actually received.
    XCTAssertTrue(result.ticketLines.contains { $0.contains("PRINTERDRIVER SELF-TEST") })
    XCTAssertTrue(result.ticketLines.contains { $0.contains("CHARSET") })
    XCTAssertTrue(bench.scriptedReceivedContains("PRINTERDRIVER SELF-TEST"))
    XCTAssertTrue(bench.scriptedReceivedContains("V:"))
    XCTAssertEqual(bench.scriptedCuts, 1)

    // The detection report the paper carries.
    XCTAssertEqual(result.detection.completion.mechanism, .gsParenH)
    XCTAssertEqual(result.detection.completion.gradeCeiling, .aJobLevelConfirmation)
    XCTAssertEqual(result.detection.media.printableWidthDots, 576)
    XCTAssertEqual(result.detection.media.charactersPerLine, 48)
    XCTAssertEqual(result.detection.endpoint, "bench")
    XCTAssertTrue(result.detection.degradations.isEmpty)
    XCTAssertTrue(result.detection.provenanceSummary.contains("GS(H) fn48"))
    XCTAssertEqual(result.detection.selection.abiName, "Documented")

    // A self-test is an ordinary job under an ordinary key: the same key twice prints
    // once.
    let again = try bench.selfTest(SelfTestOptions(key: "selftest-fixed"))
    XCTAssertEqual(bench.scriptedCuts, 2)
    let third = try bench.selfTest(SelfTestOptions(key: "selftest-fixed"))
    XCTAssertEqual(bench.scriptedCuts, 2)
    XCTAssertTrue(again.job === third.job)
  }

  func testAutoDetectClassifiesLoopbackListenersAndPrintsNothing() throws {
    let answering = try XCTUnwrap(pd_test_listener_start("ok"))
    let silent = try XCTUnwrap(pd_test_listener_start("silent"))
    let gone = try XCTUnwrap(pd_test_listener_start("ok"))
    defer {
      pd_test_listener_destroy(answering)
      pd_test_listener_destroy(silent)
      pd_test_listener_destroy(gone)
    }
    let refusedPort = pd_test_listener_port(gone)
    pd_test_listener_stop(gone)  // the port is now closed: a refusal, deterministically

    let driver = try PrinterDriver(fsyncDisabled: true)
    let options = AutoDetectOptions(
      endpoints: [
        "127.0.0.1:\(pd_test_listener_port(answering))",
        "127.0.0.1:\(pd_test_listener_port(silent))",
        "127.0.0.1:\(refusedPort)",
      ],
      connectTimeoutMilliseconds: 500,
      responseTimeoutMilliseconds: 150)

    let found = try driver.autoDetect(options)
    XCTAssertEqual(found.count, 3)

    let talker = try XCTUnwrap(found.first { $0.status == .answered })
    XCTAssertTrue(talker.isPortOpen)
    XCTAssertFalse(talker.isFromCache)
    XCTAssertEqual(talker.summary.identity.model, "TM-T88V")
    // GS I is a string the firmware chooses, and at least one family ships answering as
    // somebody else's model.
    XCTAssertFalse(talker.summary.identity.isTrusted)
    XCTAssertEqual(talker.summary.completion.mechanism, .gsParenH)
    XCTAssertEqual(talker.summary.completion.gradeCeiling, .aJobLevelConfirmation)
    // The printless probe promotes the flag and not its provenance.
    XCTAssertEqual(talker.summary.completion.provenance, .unverified)
    XCTAssertTrue(talker.summary.degradations.contains { $0.contains("empty buffer") })
    XCTAssertFalse(talker.dleEotHex.isEmpty)

    let quiet = try XCTUnwrap(found.first { $0.status == .silent })
    XCTAssertTrue(quiet.isPortOpen)
    XCTAssertTrue(quiet.dleEotHex.isEmpty)

    let dead = try XCTUnwrap(found.first { $0.status == .unreachable })
    XCTAssertFalse(dead.isPortOpen)
    XCTAssertTrue(dead.summary.profileID.isEmpty)

    // The whole point: not one printable byte reached either live device.
    XCTAssertEqual(pd_test_listener_print_data_bytes(answering), 0)
    XCTAssertEqual(pd_test_listener_print_data_bytes(silent), 0)

    pd_test_listener_stop(answering)
    pd_test_listener_stop(silent)
  }

  func testAutoDetectStreamDeliversCandidatesAsTheyAreFound() throws {
    let answering = try XCTUnwrap(pd_test_listener_start("ok"))
    defer { pd_test_listener_destroy(answering) }
    let driver = try PrinterDriver(fsyncDisabled: true)
    let options = AutoDetectOptions(
      endpoints: ["127.0.0.1:\(pd_test_listener_port(answering))"],
      connectTimeoutMilliseconds: 500,
      responseTimeoutMilliseconds: 150)

    runAsync(in: self) {
      var seen: [DetectedPrinter] = []
      for try await candidate in driver.autoDetectStream(options) {
        seen.append(candidate)
      }
      XCTAssertEqual(seen.count, 1)
      XCTAssertEqual(seen.first?.status, .answered)
    }
    XCTAssertEqual(pd_test_listener_print_data_bytes(answering), 0)
    pd_test_listener_stop(answering)
  }

  func testDiscoverSweepsALoopbackAddressAndWritesOnlyDleEot() throws {
    let answering = try XCTUnwrap(pd_test_listener_start("ok"))
    defer { pd_test_listener_destroy(answering) }
    let driver = try PrinterDriver(fsyncDisabled: true)

    let options = DiscoverOptions(
      subnetCIDR: "127.0.0.1/32",
      port: pd_test_listener_port(answering),
      connectTimeoutMilliseconds: 500,
      responseTimeoutMilliseconds: 300)

    // The per-device callback runs on a sweep worker thread, so the collector is a
    // reference type with its own lock rather than a captured `var`.
    let streamed = DeviceCollector()
    let found = try driver.discover(options) { streamed.append($0) }
    XCTAssertEqual(found.count, 1)
    XCTAssertEqual(streamed.count, 1)
    XCTAssertEqual(found.first?.ip, "127.0.0.1")
    XCTAssertTrue(found.first?.isPortOpen == true)
    XCTAssertTrue(found.first?.didAnswer == true)
    // The scripted device's DLE EOT 1 answer: online, drawer pin high.
    XCTAssertEqual(found.first?.dleEotHex, "16")
    XCTAssertEqual(pd_test_listener_print_data_bytes(answering), 0)

    // A CIDR wider than /16 is a mistyped subnet, not a venue, and is refused.
    XCTAssertThrowsError(try driver.discover(DiscoverOptions(subnetCIDR: "10.0.0.0/8")))

    pd_test_listener_stop(answering)
  }

  func testTheDetectionEnumsMirrorTheAbiSpellings() {
    XCTAssertEqual(ProfileSelection.allCases.count, Int(PD_PROFILE_SELECTION_COUNT.rawValue))
    XCTAssertEqual(DetectionStatus.allCases.count, Int(PD_DETECTION_STATUS_COUNT.rawValue))
    for value in ProfileSelection.allCases {
      XCTAssertEqual(
        value.abiName,
        String(cString: pd_test_cpp_enum_name(PD_TEST_ENUM_PROFILE_SELECTION, Int32(value.rawValue))))
    }
    for value in DetectionStatus.allCases {
      XCTAssertEqual(
        value.abiName,
        String(cString: pd_test_cpp_enum_name(PD_TEST_ENUM_DETECTION_STATUS, Int32(value.rawValue))))
    }
  }
}
