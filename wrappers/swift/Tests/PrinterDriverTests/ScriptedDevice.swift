import CPrinterDriver
import CPrinterDriverTestSupport
import XCTest

@testable import PrinterDriver

// The scripted device is a C++ transport factory, which is why it cannot live behind
// pd.h: a C caller has no way to describe one. capi/tests/pd_test_support.cpp supplies it
// to this target and to no other, so nothing an app links carries a test double.

/// The device behaviours `pd_add_printer_scripted` can attach.
enum ScriptedDevice: String {
  /// `GS ( H` printer, healthy, answers everything. Jobs reach Done at `cutFaultFree`.
  case healthy = "ok"
  /// Queued `GS r 1` fence only; jobs cap at `cutProcessed`.
  case queuedFenceOnly = "gsr1"
  /// Accepts bytes, never answers the completion marker: jobs end `unknown`.
  case silent = "silent"
  /// Reports paper out, so strict preflight refuses before any payload byte.
  case paperOut = "paperout"
  /// The connection itself fails.
  case refusesConnection = "refuse"

  // M14 — docs/cash-drawer.md. A drawer is a solenoid latch plus a microswitch, and
  // each of these holds one of that document's cases still.

  /// Documented Epson-style 24 V port, working switch, polarity already calibrated.
  case drawer = "drawer"
  /// The same hardware before anybody has measured which level means open.
  case drawerUncalibrated = "drawer-uncalibrated"
  /// The pulse goes out and the switch never moves: a locked drawer, a jam, a wrong
  /// channel, or a cable that fits and is not wired for this printer.
  case drawerLocked = "drawer-locked"
  /// An unclassified 6P6C socket. Every pulse is refused with zero bytes written.
  case drawerUnknownPort = "drawer-unknown-port"

  // M19 — docs/receipt-dsl.md "Degradation rules".

  /// A healthy `GS ( H` printer whose profile has no `GS k` path, so a `barcode` block is
  /// declared in the render report and omitted rather than printed as literal text.
  case noBarcode = "no-barcode"
}

extension PrinterDriver {
  /// Attaches a printer whose transport is an in-process scripted device.
  func scriptedPrinter(id: String, _ script: ScriptedDevice) throws -> Printer {
    guard let handle = pd_add_printer_scripted(core.handle, id, script.rawValue) else {
      throw PrinterDriverError("the scripted device \(script.rawValue) was refused")
    }
    return Printer(core: core, handle: handle)
  }
}

extension Printer {
  /// How many payload bytes the scripted device behind this printer actually received.
  var scriptedPrintDataBytes: Int { pd_test_print_data_bytes(handle) }

  /// How many cuts it performed.
  var scriptedCuts: Int { pd_test_cuts(handle) }

  /// Whether the byte stream it received contains `needle`.
  func scriptedReceivedContains(_ needle: String) -> Bool {
    pd_test_received_contains(handle, needle) != 0
  }

  /// M14. How many `ESC p` pulses actually reached the device — the number a refusal
  /// test has to watch stay at zero.
  var scriptedDrawerKicks: Int { pd_test_drawer_kicks(handle) }

  /// M14. Where the scripted microswitch currently sits.
  var scriptedDrawerIsOpen: Bool { pd_test_drawer_is_open(handle) != 0 }

  /// M14. An operator's hand: moves the drawer without going through the printer, which
  /// is what the non-destructive polarity calibration asks for.
  func scriptedSetDrawerOpen(_ open: Bool) {
    pd_test_set_drawer_open(handle, open ? 1 : 0)
  }
}

/// Drives an `async` body from a synchronous test so a hung await fails on a timeout
/// instead of wedging the whole run.
func runAsync(
  timeout: TimeInterval = 15,
  in testCase: XCTestCase,
  file: StaticString = #filePath,
  line: UInt = #line,
  _ body: @escaping @Sendable () async throws -> Void
) {
  let finished = testCase.expectation(description: "async body at \(file):\(line)")
  Task {
    do {
      try await body()
    } catch {
      XCTFail("async body threw \(error)", file: file, line: line)
    }
    finished.fulfill()
  }
  testCase.wait(for: [finished], timeout: timeout)
}
