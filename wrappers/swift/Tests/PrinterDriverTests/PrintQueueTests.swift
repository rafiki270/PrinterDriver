import CPrinterDriver
import XCTest

@testable import PrinterDriver

/// M13b. The print-queue addon through the ABI (docs/sdk-spec.md §12).
///
/// One happy path, driven against the real engine over the real `pd_queue_*` surface —
/// nothing is mocked at the Swift level, so what is under test is the binding plus the
/// addon, not a rehearsal of the binding's own assumptions.
final class PrintQueueTests: XCTestCase {

  func testEnqueuedJobDrainsThroughTheSameEngineAndEarnsTheSameGrade() throws {
    let driver = try PrinterDriver(fsyncDisabled: true)
    let kitchen = try driver.scriptedPrinter(id: "kitchen", .healthy)
    let queue = try PrintQueue(driver: driver, policy: QueuePolicy())

    XCTAssertFalse(queue.isPaused(kitchen.id))
    XCTAssertFalse(queue.isBlocked(kitchen.id))

    let job = try queue.enqueue(
      Payload.text(["QUEUED TICKET"]), to: kitchen,
      options: QueueOptions(key: "queued-1"))
    XCTAssertEqual(job.key, "queued-1")

    runAsync(in: self) {
      switch await job.result {
      case .done(let confidence, let grade, _, let method):
        // Rule 3 of §12, observable: a queued job goes down the identical engine path a
        // direct print takes, so it earns the identical claim from the identical fence.
        XCTAssertEqual(confidence, .cutFaultFree)
        XCTAssertEqual(grade, .aJobLevelConfirmation)
        XCTAssertEqual(method, "GS(H) fn48")
      case .failed(let reason, _):
        XCTFail("queued job failed: \(reason)")
      case .unknown:
        XCTFail("queued job ended unknown")
      }
    }

    // Rule 2: the key was claimed in the driver's own index at enqueue time, so a direct
    // print of the same key finds the queued job instead of producing a second receipt.
    let deduped = try kitchen.print(Payload.text(["DUPLICATE"]),
                                    options: JobOptions(key: "queued-1"))
    XCTAssertEqual(deduped.id, job.id)

    XCTAssertEqual(queue.pending(), 0)
    XCTAssertEqual(queue.pending(kitchen.id), 0)
    XCTAssertEqual(queue.expiredCount, 0)
    XCTAssertEqual(queue.overflowCount, 0)
    queue.tick()

    // Operator hold is independent of anything the device is reporting.
    queue.pause(kitchen.id)
    XCTAssertTrue(queue.isPaused(kitchen.id))
    queue.resume(kitchen.id)
    XCTAssertFalse(queue.isPaused(kitchen.id))
  }
}
