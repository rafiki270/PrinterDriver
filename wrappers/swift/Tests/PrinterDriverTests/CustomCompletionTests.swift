import CPrinterDriver
import Foundation
import XCTest

@testable import PrinterDriver

/// Custom completion method registration, end to end (docs/api.md §16).
///
/// A made-up vendor idle scheme, "acme.x-idle": the host sends `ESC 'x'` + the job's
/// four-character verification token behind the payload, and an idle device echoes
/// `ESC 'y'` + the same token. Registered as a completion method, that ack scheme drives a
/// grade-A completion path with no core release — and the Swift binding of the fence and
/// matcher callbacks is what this proves, on top of the C test that proves the ABI.
final class CustomCompletionTests: XCTestCase {

  /// A printer that answers the strict-preflight DLE EOT queries and the acme idle fence,
  /// delivered from its own queue (pd.h forbids feeding bytes from inside `write`).
  final class AcmeIdlePrinter: BluetoothTransport, @unchecked Sendable {
    let endpointDescription: String
    private let queue = DispatchQueue(label: "test.acme-idle")
    private let lock = NSLock()
    private var link: BluetoothLink?
    private var received: [UInt8] = []
    private(set) var bytesWritten = 0
    /// False models a device that stays busy and never idles, so the fence is never
    /// confirmed and the job ends Unknown.
    var answersIdle = true

    init(endpointDescription: String = "bt-spp:acme-idle") {
      self.endpointDescription = endpointDescription
    }

    func open(link: BluetoothLink) throws {
      lock.lock(); self.link = link; lock.unlock()
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

    func close() {}

    func receivedContains(_ needle: String) -> Bool {
      lock.lock(); defer { lock.unlock() }
      let haystack = received
      let pattern = Array(needle.utf8)
      guard !pattern.isEmpty, haystack.count >= pattern.count else { return false }
      for start in 0...(haystack.count - pattern.count)
      where Array(haystack[start..<(start + pattern.count)]) == pattern {
        return true
      }
      return false
    }

    private func replies(to chunk: [UInt8]) -> [UInt8] {
      var out: [UInt8] = []
      var i = 0
      while i < chunk.count {
        // DLE EOT n — the real-time status strict preflight asks for.
        if chunk[i] == 0x10, i + 2 < chunk.count, chunk[i + 1] == 0x04 {
          out.append(chunk[i + 2] == 1 ? 0x16 : 0x12)
          i += 3
          continue
        }
        // ESC 'x' + 4-char token — the acme idle fence. Echo ESC 'y' + the token.
        if chunk[i] == 0x1B, i + 5 < chunk.count, chunk[i + 1] == 0x78 {
          if answersIdle {
            out.append(contentsOf: [0x1B, 0x79])
            out.append(contentsOf: chunk[(i + 2)...(i + 5)])
          }
          i += 6
          continue
        }
        i += 1
      }
      return out
    }
  }

  /// The made-up scheme, as a registered method: grade A, physical printer.
  private func acmeIdle() -> CompletionMethod {
    CompletionMethod(
      id: "acme.x-idle",
      grade: .aJobLevelConfirmation,
      authority: .physicalPrinter,
      fenceBytes: { token in [0x1B, 0x78] + Array(token.utf8) },
      matcher: { bytes in
        var i = 0
        while i + 1 < bytes.count {
          if bytes[i] == 0x1B, bytes[i + 1] == 0x79 {
            if i + 6 > bytes.count { return .needMore }
            return .matched(String(decoding: bytes[(i + 2)..<(i + 6)], as: UTF8.self))
          }
          i += 1
        }
        return bytes.last == 0x1B ? .needMore : .notMine
      })
  }

  private let receipt = Payload.text(["ACME IDLE TICKET", "Order 42"])

  func testARegisteredVendorCompletionMethodEarnsItsDeclaredGrade() throws {
    let driver = try PrinterDriver(fsyncDisabled: true)
    try driver.register(completionMethod: acmeIdle())

    let device = AcmeIdlePrinter()
    let printer = try driver.printer(
      bluetooth: device, width: .dots576, profile: "vendoridle:acme.x-idle")
    XCTAssertEqual(printer.completionMechanism, .vendorIdle)

    let job = try printer.print(receipt, options: JobOptions(key: "acme-1"))
    runAsync(in: self) {
      switch await job.result {
      case .done(let confidence, let grade, let authority, let method):
        // The registered claim, attributed by id.
        XCTAssertEqual(grade, .aJobLevelConfirmation)
        XCTAssertEqual(grade.letter, "A")
        XCTAssertEqual(authority, .physicalPrinter)
        XCTAssertEqual(method, "acme.x-idle")
        // A vendor idle fence confirms print + cut-processed, not a fault-free blade.
        XCTAssertEqual(confidence, .cutProcessed)
      case .failed(let reason, _):
        XCTFail("expected done, got failed(\(reason))")
      case .unknown:
        XCTFail("expected done, got unknown")
      }
    }

    XCTAssertGreaterThan(device.bytesWritten, 0)
    XCTAssertTrue(device.receivedContains("ACME IDLE TICKET"))
    // The custom fence promotes its per-job token to a resolvable verification identifier,
    // exactly like GS ( H: the ticket resolves back to this job.
    let token = try XCTUnwrap(job.printToken)
    XCTAssertEqual(token.count, 4)
    XCTAssertNotNil(driver.job(token: token))
  }

  func testADeviceThatNeverIdlesLeavesTheJobUnknown() throws {
    let driver = try PrinterDriver(fsyncDisabled: true)
    try driver.register(completionMethod: acmeIdle())

    let device = AcmeIdlePrinter()
    device.answersIdle = false  // the fence is sent, never echoed
    let printer = try driver.printer(
      bluetooth: device, width: .dots576, profile: "vendoridle:acme.x-idle")

    let job = try printer.print(
      receipt, options: JobOptions(key: "acme-silent-1", timeoutMilliseconds: 900))
    runAsync(in: self) {
      switch await job.result {
      case .done:
        XCTFail("a device that never idled must not report done")
      case .failed(let reason, _):
        XCTFail("expected unknown, got failed(\(reason))")
      case .unknown:
        break  // bytes went out, no ack came back: the honest answer
      }
    }
    XCTAssertGreaterThan(device.bytesWritten, 0)
  }

  func testARegistrationIsRefusedForABadRecord() throws {
    let driver = try PrinterDriver(fsyncDisabled: true)
    try driver.register(completionMethod: acmeIdle())
    // A duplicate id is refused.
    XCTAssertThrowsError(try driver.register(completionMethod: acmeIdle()))
  }
}
