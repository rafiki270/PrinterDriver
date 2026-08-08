import CPrinterDriver
import CPrinterDriverTestSupport
import CoreGraphics
import XCTest

@testable import PrinterDriver

/// End-to-end through the real engine.
///
/// Nothing here is mocked at the Swift level: every test drives the actual C++ core over
/// the real C ABI and only swaps the socket for an in-process scripted device, so what is
/// under test is the wrapper plus the engine, not a rehearsal of the wrapper's own
/// assumptions.
final class PrintJobTests: XCTestCase {

  private func makeDriver() throws -> PrinterDriver {
    try PrinterDriver(fsyncDisabled: true)
  }

  private let receipt = Payload.text(["MY RESTAURANT", "Order 7F3A-92C1", "1x Soup"])

  // MARK: - The happy path

  func testSubmitReachesDoneWithTheConfidenceTheFenceEarned() throws {
    let driver = try makeDriver()
    let kitchen = try driver.scriptedPrinter(id: "kitchen", .healthy)
    XCTAssertEqual(kitchen.completionMechanism, .gsParenH)

    let job = try kitchen.print(receipt, options: JobOptions(key: "order-7F3A-92C1#kitchen-1"))
    XCTAssertEqual(job.key, "order-7F3A-92C1#kitchen-1")
    XCTAssertEqual(job.attempt, 1)

    runAsync(in: self) {
      switch await job.result {
      case .done(let confidence, let authority):
        // A GS ( H printer can prove the cut went through fault-free; the wrapper reports
        // exactly that and no more.
        XCTAssertEqual(confidence, .cutFaultFree)
        XCTAssertEqual(authority, .gsParenH)
      case .failed(let reason, _):
        XCTFail("expected done, got failed(\(reason))")
      case .unknown:
        XCTFail("expected done, got unknown")
      }
    }

    XCTAssertTrue(job.isTerminal)
    XCTAssertEqual(job.state, .doneSoftware)
    XCTAssertGreaterThan(kitchen.scriptedPrintDataBytes, 0)
    XCTAssertEqual(kitchen.scriptedCuts, 1)
  }

  func testAPrinterWithOnlyTheQueuedFenceStopsAtCutProcessed() throws {
    let driver = try makeDriver()
    let printer = try driver.scriptedPrinter(id: "gsr1", .queuedFenceOnly)
    XCTAssertEqual(printer.completionMechanism, .gsR1)
    let job = try printer.print(receipt, options: JobOptions(key: "gsr1-1"))

    runAsync(in: self) {
      guard case .done(let confidence, let authority) = await job.result else {
        return XCTFail("expected done")
      }
      // An ordered GS r 1 after the cut proves ordering, not a clean cutter — so the
      // ladder stops one rung below the GS ( H printer's.
      XCTAssertEqual(confidence, .cutProcessed)
      XCTAssertEqual(authority, .gsR1)
    }
  }

  // MARK: - The tri-state

  func testASilentPrinterEndsUnknownAndNeverFailedOrDone() throws {
    let driver = try makeDriver()
    let printer = try driver.scriptedPrinter(id: "silent", .silent)
    let job = try printer.print(receipt, options: JobOptions(key: "silent-1"))

    runAsync(in: self) {
      switch await job.result {
      case .unknown(let confidence):
        // Bytes went out and were never acknowledged. This is the case that must not be
        // collapsed into either bucket: doing so is what prints a second kitchen ticket.
        XCTAssertEqual(confidence, .printerHealthy)
      case .done:
        XCTFail("a timeout must never be reported as done")
      case .failed(let reason, _):
        XCTFail("a completion timeout is not a failure, got failed(\(reason))")
      }
    }
    XCTAssertGreaterThan(printer.scriptedPrintDataBytes, 0, "the payload did reach the device")
  }

  func testStrictPreflightRefusesBeforeAnyPayloadByte() throws {
    let driver = try makeDriver()
    let printer = try driver.scriptedPrinter(id: "paperout", .paperOut)
    let job = try printer.print(receipt, options: JobOptions(key: "paperout-1"))

    runAsync(in: self) {
      guard case .failed(let reason, _) = await job.result else {
        return XCTFail("expected failed")
      }
      XCTAssertEqual(reason, .preflightPaperOut)
    }
    XCTAssertEqual(
      printer.scriptedPrintDataBytes, 0, "a refused job must not put ink on paper")
  }

  func testAnUnreachableTransportFailsRatherThanHangs() throws {
    let driver = try makeDriver()
    let printer = try driver.scriptedPrinter(id: "refuse", .refusesConnection)
    let job = try printer.print(receipt, options: JobOptions(key: "refuse-1"))

    runAsync(in: self) {
      guard case .failed(let reason, let confidence) = await job.result else {
        return XCTFail("expected failed")
      }
      XCTAssertEqual(reason, .transportUnreachable)
      XCTAssertEqual(confidence, .transportAccepted)
    }
  }

  // MARK: - Dedupe

  func testResubmittingAKeyReturnsTheSameJobAndPrintsNothing() throws {
    let driver = try makeDriver()
    let kitchen = try driver.scriptedPrinter(id: "kitchen", .healthy)
    let key = "order-7F3A-92C1#kitchen-1"

    let first = try kitchen.print(receipt, options: JobOptions(key: key))
    kitchen.drain()
    let bytesAfterFirst = kitchen.scriptedPrintDataBytes
    let cutsAfterFirst = kitchen.scriptedCuts
    XCTAssertGreaterThan(bytesAfterFirst, 0)

    let second = try kitchen.print(receipt, options: JobOptions(key: key))
    kitchen.drain()

    // The ABI maps one job to one stable handle, and the wrapper interns handles, so the
    // dedupe is visible as object identity rather than as a copy that merely compares
    // equal.
    XCTAssertTrue(first === second, "the same key must hand back the same job object")
    XCTAssertEqual(driver.job(key: key).map { $0 === first }, true)
    XCTAssertEqual(kitchen.scriptedPrintDataBytes, bytesAfterFirst, "nothing may be reprinted")
    XCTAssertEqual(kitchen.scriptedCuts, cutsAfterFirst)
  }

  func testForceReprintIsTheOnlyWayToPrintAKeyTwiceAndItMarksThePaper() throws {
    let driver = try makeDriver()
    let kitchen = try driver.scriptedPrinter(id: "kitchen", .healthy)
    let key = "order-7F3A-92C1#kitchen-1"

    _ = try kitchen.print(receipt, options: JobOptions(key: key))
    kitchen.drain()
    let bytesAfterFirst = kitchen.scriptedPrintDataBytes

    let reprint = try kitchen.forceReprint(key: key)
    kitchen.drain()

    XCTAssertEqual(reprint.attempt, 2)
    XCTAssertGreaterThan(kitchen.scriptedPrintDataBytes, bytesAfterFirst)
    XCTAssertTrue(kitchen.scriptedReceivedContains("REPRINT / POSSIBLE DUPLICATE"))
    XCTAssertTrue(kitchen.scriptedReceivedContains("PRINT ATTEMPT: 2"))
  }

  func testAnUnknownKeyIsNotAnError() throws {
    let driver = try makeDriver()
    XCTAssertNil(driver.job(key: "never-submitted"))
  }

  // MARK: - Closure form

  func testClosureFormFiresTheTerminalCompletionExactlyOnce() throws {
    let driver = try makeDriver()
    let kitchen = try driver.scriptedPrinter(id: "kitchen", .healthy)

    let settled = expectation(description: "terminal")
    settled.assertForOverFulfill = true
    let progressed = expectation(description: "progress")
    progressed.assertForOverFulfill = false

    let counter = CallCounter()
    let job = try kitchen.print(
      receipt,
      options: JobOptions(key: "closure-1"),
      onProgress: { _ in
        counter.countProgress()
        progressed.fulfill()
      },
      completion: { result in
        counter.countTerminal()
        XCTAssertEqual(result.outcome, .done)
        settled.fulfill()
      })

    wait(for: [progressed, settled], timeout: 15)
    XCTAssertEqual(job.state, .doneSoftware)

    // Give anything late a chance to arrive before counting, then check the whole
    // observation contract in one go: one terminal call, progress before it, and no
    // progress after it.
    let quiesced = expectation(description: "quiesced")
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { quiesced.fulfill() }
    wait(for: [quiesced], timeout: 5)

    XCTAssertEqual(counter.terminalCalls, 1, "the terminal callback must fire exactly once")
    XCTAssertGreaterThan(counter.progressCalls, 0)
    XCTAssertEqual(
      counter.progressCallsAfterTerminal, 0, "no event may follow the terminal callback")
  }

  func testObservingAFinishedJobStillDeliversHistoryAndOneTerminalCall() throws {
    let driver = try makeDriver()
    let kitchen = try driver.scriptedPrinter(id: "kitchen", .healthy)
    let job = try kitchen.print(receipt, options: JobOptions(key: "late-1"))
    runAsync(in: self) { _ = await job.result }
    XCTAssertTrue(job.isTerminal)

    // Attaching after the fact must not be a silent no-op: a UI that comes back from the
    // background needs the history and the answer, not an empty stream.
    let settled = expectation(description: "terminal")
    settled.assertForOverFulfill = true
    var replayed: [JobState] = []
    let lock = NSLock()
    job.observe(
      onProgress: { event in
        lock.lock()
        replayed.append(event.state)
        lock.unlock()
      },
      completion: { _ in settled.fulfill() })
    wait(for: [settled], timeout: 15)

    lock.lock()
    let states = replayed
    lock.unlock()
    XCTAssertEqual(states.first, .queued)
    XCTAssertEqual(states.last, .doneSoftware)
  }

  // MARK: - AsyncStream

  func testEventStreamRunsInOrderAndFinishesAfterTheTerminalEvent() throws {
    let driver = try makeDriver()
    let kitchen = try driver.scriptedPrinter(id: "kitchen", .healthy)
    let job = try kitchen.print(receipt, options: JobOptions(key: "stream-1"))

    runAsync(in: self) {
      var states: [JobState] = []
      var timestamps: [UInt64] = []
      for await event in job.events {
        states.append(event.state)
        timestamps.append(event.monotonicMilliseconds)
        XCTAssertNil(event.reason, "a healthy job carries no failure reason")
      }

      // The exact machine of techspec §5.1, in order, ending terminal.
      XCTAssertEqual(
        states,
        [
          .queued, .preflightOk, .sendStarted, .bytesSent, .printConfirmed,
          .cutCommandProcessed, .doneSoftware,
        ])
      XCTAssertEqual(timestamps, timestamps.sorted(), "the steady clock must not go backwards")
      // The stream ended on its own — that is what leaving the for-await loop means — and
      // it ended after the terminal event rather than before it.
      XCTAssertEqual(states.last, .doneSoftware)
    }
  }

  func testSubscribingWhileTheJobIsAlreadyRunningNeverReordersEvents() throws {
    // Regression guard for the subscription window. `pd_subscribe_job` makes the callback
    // live before it replays the history, so a worker emitting at that moment can deliver
    // a live event ahead of the recorded ones. It reproduced roughly one run in three
    // before JobEventTrampoline started separating the two sources by thread; twenty jobs
    // makes a reappearance overwhelmingly likely to be caught.
    let driver = try makeDriver()
    let kitchen = try driver.scriptedPrinter(id: "kitchen", .healthy)
    let expected: [JobState] = [
      .queued, .preflightOk, .sendStarted, .bytesSent, .printConfirmed,
      .cutCommandProcessed, .doneSoftware,
    ]

    for attempt in 0..<20 {
      let job = try kitchen.print(receipt, options: JobOptions(key: "ordering-\(attempt)"))
      runAsync(in: self) {
        let states = await collect(job.events)
        XCTAssertEqual(states, expected, "attempt \(attempt) saw events out of order")
      }
    }
  }

  func testTwoStreamsOnOneJobSeeTheSameSequence() throws {
    let driver = try makeDriver()
    let kitchen = try driver.scriptedPrinter(id: "kitchen", .healthy)
    let job = try kitchen.print(receipt, options: JobOptions(key: "stream-2"))

    runAsync(in: self) {
      async let first = collect(job.events)
      async let second = collect(job.events)
      let (a, b) = await (first, second)
      XCTAssertEqual(a, b)
      XCTAssertEqual(a.last, .doneSoftware)
    }
  }

  // MARK: - Payload tiers

  func testAllThreeTiersReachTheDeviceAndDone() throws {
    let driver = try makeDriver()
    let kitchen = try driver.scriptedPrinter(id: "kitchen", .healthy)

    let raster = try Payload.raster(
      grayscale: Data(repeating: 0x40, count: 64 * 8), width: 64, height: 8)
    let raw = Payload.raw(Data("HELLO RAW\n".utf8))

    for (index, payload) in [receipt, raster, raw].enumerated() {
      let job = try kitchen.print(payload, options: JobOptions(key: "tier-\(index)"))
      let kind = payload.kind
      runAsync(in: self) {
        let outcome = await job.result.outcome
        XCTAssertEqual(outcome, .done, "tier \(kind) did not finish")
      }
    }
    XCTAssertTrue(kitchen.scriptedReceivedContains("MY RESTAURANT"))
    XCTAssertTrue(kitchen.scriptedReceivedContains("HELLO RAW"))
    XCTAssertEqual(kitchen.scriptedCuts, 3)
  }

  func testACGImageBecomesARasterPayloadTheDeviceAccepts() throws {
    let driver = try makeDriver()
    let kitchen = try driver.scriptedPrinter(id: "kitchen", .healthy)

    let width = 64
    let height = 16
    guard
      let context = CGContext(
        data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
        space: CGColorSpaceCreateDeviceRGB(),
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue),
      let image = { () -> CGImage? in
        context.setFillColor(gray: 0, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: width / 2, height: height))
        return context.makeImage()
      }()
    else {
      return XCTFail("could not build a test image")
    }

    let payload = try Payload.raster(image)
    XCTAssertEqual(payload.kind, .raster)
    let job = try kitchen.print(payload, options: JobOptions(key: "cgimage-1"))
    runAsync(in: self) {
      let outcome = await job.result.outcome
      XCTAssertEqual(outcome, .done)
    }
    // Raster bytes arrive inside GS v 0, which the scripted device consumes as a command
    // rather than as print data, so the cut is the observable proof the job really ran.
    XCTAssertEqual(kitchen.scriptedCuts, 1)
  }

  func testProfileIdsComeFromTheABIRatherThanAHardcodedList() {
    XCTAssertTrue(PrinterDriver.profileIDs.contains("generic"))
    XCTAssertFalse(PrinterDriver.profileIDs.isEmpty)
  }
}

// MARK: - Test support

private func collect(_ stream: AsyncStream<JobEvent>) async -> [JobState] {
  var states: [JobState] = []
  for await event in stream {
    states.append(event.state)
  }
  return states
}

/// Counts callbacks from the driver's delivery queue.
private final class CallCounter: @unchecked Sendable {
  private let lock = NSLock()
  private var progress = 0
  private var terminal = 0
  private var progressAfterTerminal = 0

  func countProgress() {
    lock.lock()
    defer { lock.unlock() }
    progress += 1
    if terminal > 0 { progressAfterTerminal += 1 }
  }

  func countTerminal() {
    lock.lock()
    defer { lock.unlock() }
    terminal += 1
  }

  var progressCalls: Int {
    lock.lock()
    defer { lock.unlock() }
    return progress
  }

  var terminalCalls: Int {
    lock.lock()
    defer { lock.unlock() }
    return terminal
  }

  var progressCallsAfterTerminal: Int {
    lock.lock()
    defer { lock.unlock() }
    return progressAfterTerminal
  }
}
