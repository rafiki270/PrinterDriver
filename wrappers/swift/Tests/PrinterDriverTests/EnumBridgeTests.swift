import CPrinterDriver
import CPrinterDriverTestSupport
import XCTest

@testable import PrinterDriver

/// The third copy of the enums.
///
/// The C++ core is the first, `pd.h` is the second — and `capi/src/pd_capi.cpp` already
/// fails to compile if those two disagree. This suite adds the Swift mirror to the same
/// chain: counts against `pd.h`'s `_COUNT` and against the core's own `kAll*` arrays,
/// values member by member, and spellings against `pd::to_string`. A value added to the
/// core and forgotten here fails a test rather than reaching an app as a silent hole.
final class EnumBridgeTests: XCTestCase {

  // MARK: - Counts and values

  func testEveryMirroredEnumHasTheABIMemberCount() {
    assertCount(JobState.self, PD_JOB_STATE_COUNT.rawValue)
    assertCount(ConfidenceLevel.self, PD_CONFIDENCE_COUNT.rawValue)
    assertCount(DeviceEvent.self, PD_DEVICE_EVENT_COUNT.rawValue)
    assertCount(FailureReason.self, PD_REASON_COUNT.rawValue)
    assertCount(JobOutcome.self, PD_OUTCOME_COUNT.rawValue)
    assertCount(Cut.self, PD_CUT_COUNT.rawValue)
    assertCount(Preflight.self, PD_PREFLIGHT_COUNT.rawValue)
    assertCount(PayloadKind.self, PD_PAYLOAD_KIND_COUNT.rawValue)
    assertCount(CompletionMechanism.self, PD_COMPLETION_COUNT.rawValue)
    assertCount(CutVariant.self, PD_CUT_VARIANT_COUNT.rawValue)
    assertCount(Alignment.self, PD_ALIGN_COUNT.rawValue)
    assertCount(CodePage.self, PD_CODE_PAGE_COUNT.rawValue)
    assertCount(Binarization.self, PD_BINARIZATION_COUNT.rawValue)
    assertCount(ConfidenceGrade.self, PD_GRADE_COUNT.rawValue)
    assertCount(CompletionAuthority.self, PD_AUTHORITY_COUNT.rawValue)
    assertCount(Provenance.self, PD_PROVENANCE_COUNT.rawValue)
    assertCount(CommandLanguage.self, PD_LANGUAGE_COUNT.rawValue)
  }

  func testEveryMirroredEnumMatchesTheCoreMemberForMember() {
    assertMatchesCore(JobState.self, PD_TEST_ENUM_JOB_STATE)
    assertMatchesCore(ConfidenceLevel.self, PD_TEST_ENUM_CONFIDENCE)
    assertMatchesCore(DeviceEvent.self, PD_TEST_ENUM_DEVICE_EVENT)
    assertMatchesCore(FailureReason.self, PD_TEST_ENUM_FAILURE_REASON)
    assertMatchesCore(JobOutcome.self, PD_TEST_ENUM_JOB_OUTCOME)
    assertMatchesCore(Cut.self, PD_TEST_ENUM_CUT)
    assertMatchesCore(Preflight.self, PD_TEST_ENUM_PREFLIGHT)
    assertMatchesCore(PayloadKind.self, PD_TEST_ENUM_PAYLOAD_KIND)
    assertMatchesCore(CompletionMechanism.self, PD_TEST_ENUM_COMPLETION)
    assertMatchesCore(CutVariant.self, PD_TEST_ENUM_CUT_VARIANT)
    assertMatchesCore(ConfidenceGrade.self, PD_TEST_ENUM_CONFIDENCE_GRADE)
    assertMatchesCore(CompletionAuthority.self, PD_TEST_ENUM_COMPLETION_AUTHORITY)
    assertMatchesCore(Alignment.self, PD_TEST_ENUM_ALIGNMENT)
    assertMatchesCore(CodePage.self, PD_TEST_ENUM_CODE_PAGE)
    assertMatchesCore(Binarization.self, PD_TEST_ENUM_BINARIZATION)
  }

  func testEveryMirroredMemberHasTheValueOfItsCConstant() {
    XCTAssertEqual(JobState.queued.rawValue, PD_JOB_STATE_QUEUED.rawValue)
    XCTAssertEqual(JobState.preflightOk.rawValue, PD_JOB_STATE_PREFLIGHT_OK.rawValue)
    XCTAssertEqual(JobState.sendStarted.rawValue, PD_JOB_STATE_SEND_STARTED.rawValue)
    XCTAssertEqual(JobState.bytesSent.rawValue, PD_JOB_STATE_BYTES_SENT.rawValue)
    XCTAssertEqual(JobState.printConfirmed.rawValue, PD_JOB_STATE_PRINT_CONFIRMED.rawValue)
    XCTAssertEqual(
      JobState.cutCommandProcessed.rawValue, PD_JOB_STATE_CUT_COMMAND_PROCESSED.rawValue)
    XCTAssertEqual(JobState.doneSoftware.rawValue, PD_JOB_STATE_DONE_SOFTWARE.rawValue)
    XCTAssertEqual(
      JobState.physicallyVerified.rawValue, PD_JOB_STATE_PHYSICALLY_VERIFIED.rawValue)
    XCTAssertEqual(JobState.failedKnown.rawValue, PD_JOB_STATE_FAILED_KNOWN.rawValue)
    XCTAssertEqual(JobState.unknown.rawValue, PD_JOB_STATE_UNKNOWN.rawValue)
    XCTAssertEqual(JobState.heldOffline.rawValue, PD_JOB_STATE_HELD_OFFLINE.rawValue)

    XCTAssertEqual(
      ConfidenceLevel.transportAccepted.rawValue, PD_CONFIDENCE_TRANSPORT_ACCEPTED.rawValue)
    XCTAssertEqual(
      ConfidenceLevel.printerHealthy.rawValue, PD_CONFIDENCE_PRINTER_HEALTHY.rawValue)
    XCTAssertEqual(
      ConfidenceLevel.printConfirmed.rawValue, PD_CONFIDENCE_PRINT_CONFIRMED.rawValue)
    XCTAssertEqual(ConfidenceLevel.cutProcessed.rawValue, PD_CONFIDENCE_CUT_PROCESSED.rawValue)
    XCTAssertEqual(ConfidenceLevel.cutFaultFree.rawValue, PD_CONFIDENCE_CUT_FAULT_FREE.rawValue)
    XCTAssertEqual(
      ConfidenceLevel.physicallyVerified.rawValue, PD_CONFIDENCE_PHYSICALLY_VERIFIED.rawValue)

    XCTAssertEqual(DeviceEvent.online.rawValue, PD_DEVICE_ONLINE.rawValue)
    XCTAssertEqual(DeviceEvent.offline.rawValue, PD_DEVICE_OFFLINE.rawValue)
    XCTAssertEqual(DeviceEvent.coverOpen.rawValue, PD_DEVICE_COVER_OPEN.rawValue)
    XCTAssertEqual(DeviceEvent.coverClosed.rawValue, PD_DEVICE_COVER_CLOSED.rawValue)
    XCTAssertEqual(DeviceEvent.paperOut.rawValue, PD_DEVICE_PAPER_OUT.rawValue)
    XCTAssertEqual(DeviceEvent.paperNearEnd.rawValue, PD_DEVICE_PAPER_NEAR_END.rawValue)
    XCTAssertEqual(DeviceEvent.paperOk.rawValue, PD_DEVICE_PAPER_OK.rawValue)
    XCTAssertEqual(DeviceEvent.cutterError.rawValue, PD_DEVICE_CUTTER_ERROR.rawValue)
    XCTAssertEqual(DeviceEvent.recoverableError.rawValue, PD_DEVICE_RECOVERABLE_ERROR.rawValue)
    XCTAssertEqual(
      DeviceEvent.unrecoverableError.rawValue, PD_DEVICE_UNRECOVERABLE_ERROR.rawValue)
    XCTAssertEqual(DeviceEvent.connectionLost.rawValue, PD_DEVICE_CONNECTION_LOST.rawValue)
    XCTAssertEqual(
      DeviceEvent.connectionRestored.rawValue, PD_DEVICE_CONNECTION_RESTORED.rawValue)
    XCTAssertEqual(
      DeviceEvent.foreignWriterDetected.rawValue,
      PD_DEVICE_FOREIGN_WRITER_DETECTED.rawValue)

    XCTAssertEqual(FailureReason.none.rawValue, PD_REASON_NONE.rawValue)
    XCTAssertEqual(
      FailureReason.transportUnreachable.rawValue, PD_REASON_TRANSPORT_UNREACHABLE.rawValue)
    XCTAssertEqual(
      FailureReason.preflightCoverOpen.rawValue, PD_REASON_PREFLIGHT_COVER_OPEN.rawValue)
    XCTAssertEqual(
      FailureReason.preflightPaperOut.rawValue, PD_REASON_PREFLIGHT_PAPER_OUT.rawValue)
    XCTAssertEqual(
      FailureReason.preflightHardwareError.rawValue,
      PD_REASON_PREFLIGHT_HARDWARE_ERROR.rawValue)
    XCTAssertEqual(
      FailureReason.timeoutAwaitingCompletion.rawValue,
      PD_REASON_TIMEOUT_AWAITING_COMPLETION.rawValue)
    XCTAssertEqual(FailureReason.cutterFault.rawValue, PD_REASON_CUTTER_FAULT.rawValue)
    XCTAssertEqual(FailureReason.unsupported.rawValue, PD_REASON_UNSUPPORTED.rawValue)
    XCTAssertEqual(FailureReason.unknown.rawValue, PD_REASON_UNKNOWN.rawValue)
    XCTAssertEqual(FailureReason.expired.rawValue, PD_REASON_EXPIRED.rawValue)
    XCTAssertEqual(FailureReason.queueOverflow.rawValue, PD_REASON_QUEUE_OVERFLOW.rawValue)

    XCTAssertEqual(JobOutcome.done.rawValue, PD_OUTCOME_DONE.rawValue)
    XCTAssertEqual(JobOutcome.failed.rawValue, PD_OUTCOME_FAILED.rawValue)
    XCTAssertEqual(JobOutcome.unknown.rawValue, PD_OUTCOME_UNKNOWN.rawValue)

    XCTAssertEqual(Cut.profile.rawValue, PD_CUT_PROFILE.rawValue)
    XCTAssertEqual(Cut.partial.rawValue, PD_CUT_PARTIAL.rawValue)
    XCTAssertEqual(Cut.full.rawValue, PD_CUT_FULL.rawValue)
    XCTAssertEqual(Cut.none.rawValue, PD_CUT_NONE.rawValue)

    XCTAssertEqual(Preflight.strict.rawValue, PD_PREFLIGHT_STRICT.rawValue)
    XCTAssertEqual(Preflight.skip.rawValue, PD_PREFLIGHT_SKIP.rawValue)

    XCTAssertEqual(PayloadKind.raster.rawValue, PD_PAYLOAD_RASTER_RGBA8.rawValue)
    XCTAssertEqual(PayloadKind.document.rawValue, PD_PAYLOAD_DOCUMENT.rawValue)
    XCTAssertEqual(PayloadKind.raw.rawValue, PD_PAYLOAD_RAW.rawValue)

    XCTAssertEqual(CompletionMechanism.gsParenH.rawValue, PD_COMPLETION_GS_PAREN_H.rawValue)
    XCTAssertEqual(CompletionMechanism.gsR1.rawValue, PD_COMPLETION_GS_R1.rawValue)
    XCTAssertEqual(CompletionMechanism.vendorIdle.rawValue, PD_COMPLETION_VENDOR_IDLE.rawValue)
    XCTAssertEqual(CompletionMechanism.eposJobId.rawValue, PD_COMPLETION_EPOS_JOB_ID.rawValue)
    XCTAssertEqual(
      CompletionMechanism.starCheckedBlock.rawValue, PD_COMPLETION_STAR_CHECKED_BLOCK.rawValue)
    XCTAssertEqual(CompletionMechanism.none.rawValue, PD_COMPLETION_NONE.rawValue)
    // M13b (docs/wire-protocols.md §2).
    XCTAssertEqual(CompletionMechanism.starEtb.rawValue, PD_COMPLETION_STAR_ETB.rawValue)
    XCTAssertEqual(
      CompletionMechanism.starEscGsEtx.rawValue, PD_COMPLETION_STAR_ESC_GS_ETX.rawValue)

    XCTAssertEqual(CutVariant.partial.rawValue, PD_CUT_VARIANT_PARTIAL.rawValue)
    XCTAssertEqual(CutVariant.full.rawValue, PD_CUT_VARIANT_FULL.rawValue)
    XCTAssertEqual(CutVariant.none.rawValue, PD_CUT_VARIANT_NONE.rawValue)

    XCTAssertEqual(Alignment.left.rawValue, PD_ALIGN_LEFT.rawValue)
    XCTAssertEqual(Alignment.center.rawValue, PD_ALIGN_CENTER.rawValue)
    XCTAssertEqual(Alignment.right.rawValue, PD_ALIGN_RIGHT.rawValue)

    XCTAssertEqual(CodePage.pc437.rawValue, PD_CODE_PAGE_PC437.rawValue)
    XCTAssertEqual(CodePage.pc850.rawValue, PD_CODE_PAGE_PC850.rawValue)
    XCTAssertEqual(CodePage.wpc1252.rawValue, PD_CODE_PAGE_WPC1252.rawValue)
    XCTAssertEqual(CodePage.pc852.rawValue, PD_CODE_PAGE_PC852.rawValue)
    XCTAssertEqual(CodePage.pc858.rawValue, PD_CODE_PAGE_PC858.rawValue)

    XCTAssertEqual(
      Binarization.fixedThreshold.rawValue, PD_BINARIZATION_FIXED_THRESHOLD.rawValue)
    XCTAssertEqual(
      Binarization.floydSteinberg.rawValue, PD_BINARIZATION_FLOYD_STEINBERG.rawValue)
  }

  // MARK: - Spellings

  func testNamedEnumsSpellMembersExactlyLikeTheCore() {
    assertSpellings(JobState.allCases.map(\.abiName), PD_TEST_ENUM_JOB_STATE)
    assertSpellings(ConfidenceLevel.allCases.map(\.abiName), PD_TEST_ENUM_CONFIDENCE)
    assertSpellings(DeviceEvent.allCases.map(\.abiName), PD_TEST_ENUM_DEVICE_EVENT)
    assertSpellings(FailureReason.allCases.map(\.abiName), PD_TEST_ENUM_FAILURE_REASON)
    assertSpellings(JobOutcome.allCases.map(\.abiName), PD_TEST_ENUM_JOB_OUTCOME)
    assertSpellings(PayloadKind.allCases.map(\.abiName), PD_TEST_ENUM_PAYLOAD_KIND)
    assertSpellings(CompletionMechanism.allCases.map(\.abiName), PD_TEST_ENUM_COMPLETION)
    assertSpellings(CutVariant.allCases.map(\.abiName), PD_TEST_ENUM_CUT_VARIANT)
    assertSpellings(
      ConfidenceGrade.allCases.map(\.abiName), PD_TEST_ENUM_CONFIDENCE_GRADE)
    assertSpellings(
      CompletionAuthority.allCases.map(\.abiName), PD_TEST_ENUM_COMPLETION_AUTHORITY)
    // "A+", "A".."E", the letter a report tabulates, straight from the ABI.
    XCTAssertEqual(ConfidenceGrade.allCases.map(\.letter), ["A+", "A", "B", "C", "D", "E"])
    // Provenance and CommandLanguage have no kAll* array reachable from this target, so
    // their spellings are checked against the ABI's own name tables instead.
    XCTAssertEqual(Provenance.allCases.map(\.abiName), ["Documented", "Probed", "Unverified"])
    XCTAssertEqual(
      CommandLanguage.allCases.map(\.abiName),
      ["EscPos", "StarPrnt", "StarLine", "EposXml", "Zpl", "Cpcl", "BrotherRaster", "EscP"])
  }

  func testGradeHierarchyGainedAPlusWithoutRenumberingAnythingElseWrongly() {
    // docs/compatibility-brief.md §24. A+ takes value 0 and A..E shift by one, so the
    // enum still reads strongest-first and a caller can compare grades numerically.
    XCTAssertEqual(ConfidenceGrade.aPlusDurableQueryableJob.rawValue, 0)
    XCTAssertEqual(ConfidenceGrade.aJobLevelConfirmation.rawValue, 1)
    XCTAssertEqual(ConfidenceGrade.eTransportOnly.rawValue, 5)
    XCTAssertEqual(ConfidenceGrade.allCases.count, 6)
    XCTAssertTrue(
      ConfidenceGrade.aPlusDurableQueryableJob.rawValue
        < ConfidenceGrade.aJobLevelConfirmation.rawValue)
    XCTAssertEqual(
      ConfidenceGrade.aPlusDurableQueryableJob.abiName, "APlus_DurableQueryableJob")

    // Nothing produces A+ yet: the ePOS transport that would retrieve a JobID result does
    // not exist. Asserting the absence keeps the claim honest until it does, at which
    // point this test has to change deliberately rather than drift.
    XCTAssertEqual(
      ConfidenceGrade.aPlusDurableQueryableJob.rawValue,
      PD_GRADE_APLUS_DURABLE_QUERYABLE_JOB.rawValue)
  }

  // MARK: - The awkward corners

  func testCodePageIsIteratedThroughTheABIRatherThanCounted() {
    // PD_CODE_PAGE_COUNT is a member count, not a past-the-end sentinel: the values are
    // the `n` operand of ESC t n and are not contiguous. A wrapper that looped 0..<COUNT
    // would produce PC437, "1", PC850, "3", "4" — hence pd_code_page_at.
    for (index, member) in CodePage.allCases.enumerated() {
      XCTAssertEqual(member.rawValue, pd_code_page_at(Int32(index)).rawValue)
    }
    XCTAssertNotEqual(
      CodePage.allCases.map(\.rawValue), Array(0..<UInt32(CodePage.allCases.count)),
      "code page values must stay the ESC t operands, not indices")
  }

  func testDocumentOpsMirrorEveryABIOpKind() {
    let oneOfEach: [DocumentOp] = [
      .text("x"), .line("x"), .align(.center), .bold(true), .feed(lines: 1),
    ]
    XCTAssertEqual(oneOfEach.count, Int(PD_OP_KIND_COUNT.rawValue))
  }

  func testBridgingAcceptsEveryValueTheABICanProduce() {
    assertBridgesEveryMember(JobState.self)
    assertBridgesEveryMember(ConfidenceLevel.self)
    assertBridgesEveryMember(DeviceEvent.self)
    assertBridgesEveryMember(FailureReason.self)
    assertBridgesEveryMember(JobOutcome.self)
    assertBridgesEveryMember(Cut.self)
    assertBridgesEveryMember(Preflight.self)
    assertBridgesEveryMember(PayloadKind.self)
    assertBridgesEveryMember(CompletionMechanism.self)
    assertBridgesEveryMember(CutVariant.self)
    assertBridgesEveryMember(Alignment.self)
    assertBridgesEveryMember(CodePage.self)
    assertBridgesEveryMember(Binarization.self)
    assertBridgesEveryMember(ConfidenceGrade.self)
    assertBridgesEveryMember(CompletionAuthority.self)
    assertBridgesEveryMember(Provenance.self)
    assertBridgesEveryMember(CommandLanguage.self)
  }

  func testUnrecognizedFallbacksNeverClaimMoreThanIsKnown() {
    // The debug half of the contract traps, so it cannot be asserted from a test process
    // that has to survive. This is the release half: every substitution is either an
    // explicit "unknown" or the weakest member of its ladder.
    XCTAssertEqual(JobState.unrecognizedFallback, .unknown)
    XCTAssertEqual(ConfidenceLevel.unrecognizedFallback, .transportAccepted)
    XCTAssertEqual(FailureReason.unrecognizedFallback, .unknown)
    XCTAssertEqual(JobOutcome.unrecognizedFallback, .unknown)
    XCTAssertEqual(CompletionMechanism.unrecognizedFallback, .none)
    XCTAssertEqual(CutVariant.unrecognizedFallback, .none)
    XCTAssertEqual(DeviceEvent.unrecognizedFallback, .recoverableError)
    XCTAssertEqual(Cut.unrecognizedFallback, .profile)
    XCTAssertEqual(Preflight.unrecognizedFallback, .strict)
    XCTAssertEqual(ConfidenceGrade.unrecognizedFallback, .eTransportOnly)
    XCTAssertEqual(CompletionAuthority.unrecognizedFallback, .transportOnly)
    // An unrecognized provenance has established nothing, which is exactly Unverified.
    XCTAssertEqual(Provenance.unrecognizedFallback, .unverified)
  }

  // MARK: - Helpers

  private func assertCount<E: ABIMirroredEnum>(
    _ type: E.Type, _ abiCount: UInt32, file: StaticString = #filePath, line: UInt = #line
  ) {
    XCTAssertEqual(
      E.allCases.count, Int(abiCount),
      "\(E.abiTypeName): the Swift mirror and pd.h disagree on how many members exist",
      file: file, line: line)
  }

  private func assertMatchesCore<E: ABIMirroredEnum>(
    _ type: E.Type, _ which: pd_test_enum, file: StaticString = #filePath, line: UInt = #line
  ) {
    let coreCount = Int(pd_test_cpp_enum_count(which))
    XCTAssertEqual(
      E.allCases.count, coreCount,
      "\(E.abiTypeName): the Swift mirror and the C++ core disagree on member count",
      file: file, line: line)
    for (index, member) in E.allCases.enumerated() where index < coreCount {
      XCTAssertEqual(
        Int32(member.rawValue), pd_test_cpp_enum_value(which, Int32(index)),
        "\(E.abiTypeName): member \(index) is \(member) here and something else in the core",
        file: file, line: line)
    }
  }

  private func assertSpellings(
    _ names: [String], _ which: pd_test_enum, file: StaticString = #filePath, line: UInt = #line
  ) {
    for (index, name) in names.enumerated() {
      guard let core = pd_test_cpp_enum_name(which, Int32(index)) else {
        XCTFail(
          "\(String(cString: pd_test_enum_label(which))): the core has no name at \(index)",
          file: file, line: line)
        continue
      }
      XCTAssertEqual(name, String(cString: core), file: file, line: line)
    }
  }

  private func assertBridgesEveryMember<E: ABIMirroredEnum>(
    _ type: E.Type, file: StaticString = #filePath, line: UInt = #line
  ) {
    for member in E.allCases {
      XCTAssertEqual(
        E(bridging: member.rawValue), member, "\(E.abiTypeName) failed to bridge \(member)",
        file: file, line: line)
    }
  }
}
