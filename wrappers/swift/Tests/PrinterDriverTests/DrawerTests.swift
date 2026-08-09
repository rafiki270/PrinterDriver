import CPrinterDriver
import CPrinterDriverTestSupport
import XCTest

@testable import PrinterDriver

/// M14 — the cash drawer, through the Swift wrapper (docs/cash-drawer.md).
///
/// The wrapper contains no drawer logic — the sequence, the refusals and the polarity all
/// live in the core — so what is under test here is that none of that is *lost* on the way
/// across: that a refusal is still a refusal, that `kickSentUnverified` does not quietly
/// become a success, and that an uncalibrated switch still reports a level rather than a
/// direction.
final class DrawerTests: XCTestCase {

  func testAVerifiedOpenIsReportedOnlyWhenTheSwitchMoves() throws {
    let driver = try PrinterDriver(fsyncDisabled: true)
    let till = try driver.scriptedPrinter(id: "till", .drawer)

    let caps = till.drawerCapabilities
    XCTAssertTrue(caps.isPresent)
    XCTAssertTrue(caps.isKickable)
    XCTAssertEqual(caps.portStandard, .epson24V6P6C)
    XCTAssertEqual(caps.kickMethod, .epsonEscP)
    XCTAssertEqual(caps.statusMethod, .gsR2)
    XCTAssertEqual(caps.voltage, 24)
    XCTAssertEqual(caps.maxCurrentMilliamps, 1000)
    XCTAssertEqual(caps.channelCount, 2)
    // The single fact that separates this arrangement from Star's.
    XCTAssertEqual(caps.sensorPin, 3)
    XCTAssertEqual(caps.electricalProvenance, .documented)
    XCTAssertEqual(caps.commandsProvenance, .documented)

    let result = till.openDrawer()
    XCTAssertEqual(result.previousState, .closed)
    XCTAssertEqual(result.state, .openVerified)
    XCTAssertTrue(result.isVerified)
    XCTAssertEqual(result.channel, 1)
    XCTAssertEqual(result.pulseMilliseconds, 200)
    XCTAssertEqual(till.scriptedDrawerKicks, 1)
    XCTAssertTrue(till.scriptedDrawerIsOpen)
    XCTAssertEqual(result.state.abiName, "OpenVerified")

    // Step 1 of the sequence: a drawer that is already out is never pulsed again.
    let again = till.openDrawer(channel: 1, pulseMilliseconds: 120)
    XCTAssertEqual(again.state, .open)
    XCTAssertEqual(again.pulseMilliseconds, 0)
    XCTAssertFalse(again.isVerified)
    XCTAssertEqual(till.scriptedDrawerKicks, 1)
  }

  func testALockedDrawerIsFailedToOpenAndNotASuccess() throws {
    let driver = try PrinterDriver(fsyncDisabled: true)
    let till = try driver.scriptedPrinter(id: "locked", .drawerLocked)

    let result = till.openDrawer(channel: 2, pulseMilliseconds: 120)
    XCTAssertEqual(result.previousState, .closed)
    XCTAssertEqual(result.state, .failedToOpen)
    XCTAssertFalse(result.isVerified)
    XCTAssertEqual(result.channel, 2)
    XCTAssertEqual(result.pulseMilliseconds, 120)
    // The pulse was real; the drawer was not.
    XCTAssertEqual(till.scriptedDrawerKicks, 1)
    XCTAssertFalse(till.scriptedDrawerIsOpen)
  }

  func testAnUnclassifiedPortIsRefusedWithoutWritingAByte() throws {
    let driver = try PrinterDriver(fsyncDisabled: true)
    let till = try driver.scriptedPrinter(id: "unclassified", .drawerUnknownPort)

    let caps = till.drawerCapabilities
    XCTAssertEqual(caps.portStandard, .unknown)
    XCTAssertFalse(caps.isKickable)

    let result = till.openDrawer()
    XCTAssertEqual(result.state, .unknown)
    XCTAssertEqual(result.pulseMilliseconds, 0)
    // RJ11/RJ12-looking drawer connectors are not a universal electrical standard, and
    // an unclassified port is never energised.
    XCTAssertEqual(till.scriptedDrawerKicks, 0)

    // Reading the switch is still safe on the same hardware: it asks a question and
    // energises nothing, which is what makes it the probe's non-destructive half.
    let reading = till.readDrawerSensor(timeoutMilliseconds: 500)
    XCTAssertTrue(reading.isAvailable)
    XCTAssertTrue(reading.didAnswer)
    XCTAssertEqual(till.scriptedDrawerKicks, 0)
  }

  func testAnUncalibratedSwitchReportsALevelAndNotADirection() throws {
    let driver = try PrinterDriver(fsyncDisabled: true)
    let till = try driver.scriptedPrinter(id: "uncalibrated", .drawerUncalibrated)
    XCTAssertFalse(till.isDrawerPolarityCalibrated)

    // The operator procedure, without the prompts.
    till.scriptedSetDrawerOpen(false)
    let shut = till.readDrawerSensor(timeoutMilliseconds: 500)
    XCTAssertTrue(shut.didAnswer)
    XCTAssertTrue(shut.needsCalibration)
    XCTAssertEqual(shut.pinHigh, false)
    // Star documents that the meaning of the level depends on the attached drawer, so
    // until it is measured there is no interpretation to report.
    XCTAssertEqual(shut.state, .unknown)

    till.scriptedSetDrawerOpen(true)
    let open = till.readDrawerSensor(timeoutMilliseconds: 500)
    XCTAssertEqual(open.pinHigh, true)
    XCTAssertNotEqual(shut.pinHigh, open.pinHigh, "a switch that moved")
    XCTAssertEqual(till.scriptedDrawerKicks, 0, "reading never fires anything")

    // In-memory driver: the calibration applies to this process and says so by returning
    // false rather than pretending it was persisted.
    XCTAssertFalse(till.calibrateDrawerPolarity(highMeansOpen: open.pinHigh ?? true))
    XCTAssertTrue(till.isDrawerPolarityCalibrated)
    XCTAssertTrue(till.drawerHighMeansOpen)

    let after = till.readDrawerSensor(timeoutMilliseconds: 500)
    XCTAssertFalse(after.needsCalibration)
    XCTAssertEqual(after.state, .open)
  }

  func testALabelPrinterHasNoDrawerAndFiresNothing() throws {
    let driver = try PrinterDriver(fsyncDisabled: true)
    // Zebra speaks ZPL and CPCL, has no drawer port, and every job on it is refused
    // before a byte is written. The drawer call is the same refusal.
    let labels = try driver.printer(.tcp(host: "192.0.2.60"), profile: "zebra_zq600_plus")

    let caps = labels.drawerCapabilities
    XCTAssertFalse(caps.isPresent)
    XCTAssertEqual(caps.kickMethod, .unsupported)
    XCTAssertFalse(caps.isKickable)

    let result = labels.openDrawer()
    XCTAssertEqual(result.state, .unknown)
    XCTAssertEqual(result.pulseMilliseconds, 0)
  }
}
