package com.printerdriver

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Pure-Kotlin tests for every enum's fromRaw <-> pd.h raw-int mapping. Deliberately
 * does not touch [com.printerdriver.internal.NativeBridge] (no `System.loadLibrary`
 * anywhere in this file) so it runs as a plain JVM unit test with no native .so and no
 * device/emulator -- see wrappers/android/README.md "Testing" and
 * .github-ci-example.yml's `test` job.
 *
 * UNVERIFIED: written but never executed -- this scaffold was authored on a host with
 * no JVM/Gradle/Android SDK. See README.md "Verification status".
 */
class EnumMappingTest {

    @Test
    fun `JobState fromRaw matches every pd_job_state value from pd h`() {
        assertEquals(JobState.QUEUED, JobState.fromRaw(0))
        assertEquals(JobState.PREFLIGHT_OK, JobState.fromRaw(1))
        assertEquals(JobState.SEND_STARTED, JobState.fromRaw(2))
        assertEquals(JobState.BYTES_SENT, JobState.fromRaw(3))
        assertEquals(JobState.PRINT_CONFIRMED, JobState.fromRaw(4))
        assertEquals(JobState.CUT_COMMAND_PROCESSED, JobState.fromRaw(5))
        assertEquals(JobState.DONE_SOFTWARE, JobState.fromRaw(6))
        assertEquals(JobState.PHYSICALLY_VERIFIED, JobState.fromRaw(7))
        assertEquals(JobState.FAILED_KNOWN, JobState.fromRaw(8))
        assertEquals(JobState.UNKNOWN, JobState.fromRaw(9))
        assertEquals(JobState.HELD_OFFLINE, JobState.fromRaw(10))
    }

    @Test
    fun `JobState fromRaw degrades unknown raw ints to UNRECOGNIZED rather than throwing or guessing`() {
        assertEquals(JobState.UNRECOGNIZED, JobState.fromRaw(11))
        assertEquals(JobState.UNRECOGNIZED, JobState.fromRaw(-1))
        assertEquals(JobState.UNRECOGNIZED, JobState.fromRaw(9999))
    }

    @Test
    fun `ConfidenceLevel fromRaw matches pd_confidence_level`() {
        assertEquals(ConfidenceLevel.TRANSPORT_ACCEPTED, ConfidenceLevel.fromRaw(0))
        assertEquals(ConfidenceLevel.PRINTER_HEALTHY, ConfidenceLevel.fromRaw(1))
        assertEquals(ConfidenceLevel.PRINT_CONFIRMED, ConfidenceLevel.fromRaw(2))
        assertEquals(ConfidenceLevel.CUT_PROCESSED, ConfidenceLevel.fromRaw(3))
        assertEquals(ConfidenceLevel.CUT_FAULT_FREE, ConfidenceLevel.fromRaw(4))
        assertEquals(ConfidenceLevel.PHYSICALLY_VERIFIED, ConfidenceLevel.fromRaw(5))
        assertEquals(ConfidenceLevel.UNRECOGNIZED, ConfidenceLevel.fromRaw(6))
    }

    @Test
    fun `DeviceEvent fromRaw matches pd_device_event`() {
        assertEquals(DeviceEvent.ONLINE, DeviceEvent.fromRaw(0))
        assertEquals(DeviceEvent.OFFLINE, DeviceEvent.fromRaw(1))
        assertEquals(DeviceEvent.COVER_OPEN, DeviceEvent.fromRaw(2))
        assertEquals(DeviceEvent.COVER_CLOSED, DeviceEvent.fromRaw(3))
        assertEquals(DeviceEvent.PAPER_OUT, DeviceEvent.fromRaw(4))
        assertEquals(DeviceEvent.PAPER_NEAR_END, DeviceEvent.fromRaw(5))
        assertEquals(DeviceEvent.PAPER_OK, DeviceEvent.fromRaw(6))
        assertEquals(DeviceEvent.CUTTER_ERROR, DeviceEvent.fromRaw(7))
        assertEquals(DeviceEvent.RECOVERABLE_ERROR, DeviceEvent.fromRaw(8))
        assertEquals(DeviceEvent.UNRECOVERABLE_ERROR, DeviceEvent.fromRaw(9))
        assertEquals(DeviceEvent.CONNECTION_LOST, DeviceEvent.fromRaw(10))
        assertEquals(DeviceEvent.CONNECTION_RESTORED, DeviceEvent.fromRaw(11))
        assertEquals(DeviceEvent.FOREIGN_WRITER_DETECTED, DeviceEvent.fromRaw(12))
        assertEquals(DeviceEvent.UNRECOGNIZED, DeviceEvent.fromRaw(13))
    }

    @Test
    fun `ConfidenceGrade fromRaw matches pd_confidence_grade`() {
        // A+ occupies raw 0, so every letter grade sits one above its pre-A+ value. A
        // wrapper that kept the old numbering would report every A-graded job as A+ --
        // an upgrade of the strongest claim in the SDK, made by an off-by-one.
        assertEquals(ConfidenceGrade.APLUS_DURABLE_QUERYABLE_JOB, ConfidenceGrade.fromRaw(0))
        assertEquals(ConfidenceGrade.A_JOB_LEVEL_CONFIRMATION, ConfidenceGrade.fromRaw(1))
        assertEquals(ConfidenceGrade.B_ORDERED_DEVICE_RESPONSE, ConfidenceGrade.fromRaw(2))
        assertEquals(ConfidenceGrade.C_DEVICE_STATUS_AROUND, ConfidenceGrade.fromRaw(3))
        assertEquals(ConfidenceGrade.D_SPOOLER_COMPLETED, ConfidenceGrade.fromRaw(4))
        assertEquals(ConfidenceGrade.E_TRANSPORT_ONLY, ConfidenceGrade.fromRaw(5))
        assertEquals(ConfidenceGrade.UNRECOGNIZED, ConfidenceGrade.fromRaw(6))
    }

    @Test
    fun `ConfidenceGrade letters match pd_confidence_grade_letter`() {
        // The letter is what a report tabulates, so it is a second, independent way for
        // the shift to go wrong: an enum could carry the right raw values and still
        // print "A" next to a grade that is not A.
        assertEquals("A+", ConfidenceGrade.APLUS_DURABLE_QUERYABLE_JOB.letter)
        assertEquals("A", ConfidenceGrade.A_JOB_LEVEL_CONFIRMATION.letter)
        assertEquals("B", ConfidenceGrade.B_ORDERED_DEVICE_RESPONSE.letter)
        assertEquals("C", ConfidenceGrade.C_DEVICE_STATUS_AROUND.letter)
        assertEquals("D", ConfidenceGrade.D_SPOOLER_COMPLETED.letter)
        assertEquals("E", ConfidenceGrade.E_TRANSPORT_ONLY.letter)
        assertEquals("?", ConfidenceGrade.UNRECOGNIZED.letter)
    }

    @Test
    fun `Provenance fromRaw matches pd_provenance`() {
        assertEquals(Provenance.DOCUMENTED, Provenance.fromRaw(0))
        assertEquals(Provenance.PROBED, Provenance.fromRaw(1))
        assertEquals(Provenance.UNVERIFIED, Provenance.fromRaw(2))
        // UNVERIFIED is a real pd_provenance member meaning "nobody has checked";
        // UNRECOGNIZED means this wrapper has never heard of the raw int. Same trap as
        // FailureReason.UNKNOWN below.
        assertEquals(Provenance.UNRECOGNIZED, Provenance.fromRaw(3))
    }

    @Test
    fun `CommandLanguage fromRaw matches pd_command_language`() {
        assertEquals(CommandLanguage.ESC_POS, CommandLanguage.fromRaw(0))
        assertEquals(CommandLanguage.STAR_PRNT, CommandLanguage.fromRaw(1))
        assertEquals(CommandLanguage.STAR_LINE, CommandLanguage.fromRaw(2))
        assertEquals(CommandLanguage.EPOS_XML, CommandLanguage.fromRaw(3))
        assertEquals(CommandLanguage.ZPL, CommandLanguage.fromRaw(4))
        assertEquals(CommandLanguage.CPCL, CommandLanguage.fromRaw(5))
        assertEquals(CommandLanguage.BROTHER_RASTER, CommandLanguage.fromRaw(6))
        // Brother ESC/P, not Epson ESC/POS. Mapping raw 7 onto ESC_POS is the exact
        // confusion docs/compatibility-brief.md §17 exists to prevent, and it would
        // route a Brother mobile down the generic ESC/POS codepath.
        assertEquals(CommandLanguage.ESC_P, CommandLanguage.fromRaw(7))
        assertEquals(CommandLanguage.UNRECOGNIZED, CommandLanguage.fromRaw(8))
    }

    @Test
    fun `CompletionAuthority fromRaw matches pd_completion_authority`() {
        assertEquals(CompletionAuthority.PHYSICAL_PRINTER, CompletionAuthority.fromRaw(0))
        assertEquals(CompletionAuthority.VENDOR_SPOOLER, CompletionAuthority.fromRaw(1))
        assertEquals(CompletionAuthority.PD_AGENT, CompletionAuthority.fromRaw(2))
        assertEquals(CompletionAuthority.PRINT_SERVER, CompletionAuthority.fromRaw(3))
        assertEquals(CompletionAuthority.TRANSPORT_ONLY, CompletionAuthority.fromRaw(4))
        assertEquals(CompletionAuthority.UNRECOGNIZED, CompletionAuthority.fromRaw(5))
    }

    @Test
    fun `FailureReason fromRaw matches pd_failure_reason, including the real UNKNOWN member`() {
        assertEquals(FailureReason.NONE, FailureReason.fromRaw(0))
        assertEquals(FailureReason.TRANSPORT_UNREACHABLE, FailureReason.fromRaw(1))
        assertEquals(FailureReason.PREFLIGHT_COVER_OPEN, FailureReason.fromRaw(2))
        assertEquals(FailureReason.PREFLIGHT_PAPER_OUT, FailureReason.fromRaw(3))
        assertEquals(FailureReason.PREFLIGHT_HARDWARE_ERROR, FailureReason.fromRaw(4))
        assertEquals(FailureReason.TIMEOUT_AWAITING_COMPLETION, FailureReason.fromRaw(5))
        assertEquals(FailureReason.CUTTER_FAULT, FailureReason.fromRaw(6))
        assertEquals(FailureReason.UNSUPPORTED, FailureReason.fromRaw(7))
        // Raw 8 is the ABI's own "reason unknown" member -- distinct from the
        // UNRECOGNIZED fallback this wrapper adds for raw ints the enum has no member
        // for at all. Both are exercised here so the distinction stays testable.
        assertEquals(FailureReason.UNKNOWN, FailureReason.fromRaw(8))
        assertEquals(FailureReason.EXPIRED, FailureReason.fromRaw(9))
        assertEquals(FailureReason.QUEUE_OVERFLOW, FailureReason.fromRaw(10))
        assertEquals(FailureReason.UNRECOGNIZED, FailureReason.fromRaw(11))
    }

    @Test
    fun `Cut fromRaw matches pd_cut`() {
        assertEquals(Cut.PROFILE, Cut.fromRaw(0))
        assertEquals(Cut.PARTIAL, Cut.fromRaw(1))
        assertEquals(Cut.FULL, Cut.fromRaw(2))
        assertEquals(Cut.NONE, Cut.fromRaw(3))
        assertEquals(Cut.UNRECOGNIZED, Cut.fromRaw(4))
    }

    @Test
    fun `Preflight fromRaw matches pd_preflight`() {
        assertEquals(Preflight.STRICT, Preflight.fromRaw(0))
        assertEquals(Preflight.SKIP, Preflight.fromRaw(1))
        assertEquals(Preflight.UNRECOGNIZED, Preflight.fromRaw(2))
    }

    @Test
    fun `Alignment fromRaw matches pd_alignment (the ESC a n operand)`() {
        assertEquals(Alignment.LEFT, Alignment.fromRaw(0))
        assertEquals(Alignment.CENTER, Alignment.fromRaw(1))
        assertEquals(Alignment.RIGHT, Alignment.fromRaw(2))
        assertEquals(Alignment.UNRECOGNIZED, Alignment.fromRaw(3))
    }

    @Test
    fun `Binarization fromRaw matches pd_binarization`() {
        assertEquals(Binarization.FIXED_THRESHOLD, Binarization.fromRaw(0))
        assertEquals(Binarization.FLOYD_STEINBERG, Binarization.fromRaw(1))
        assertEquals(Binarization.UNRECOGNIZED, Binarization.fromRaw(2))
    }

    @Test
    fun `CompletionMechanism fromRaw matches pd_completion_mechanism`() {
        assertEquals(CompletionMechanism.GS_PAREN_H, CompletionMechanism.fromRaw(0))
        assertEquals(CompletionMechanism.GS_R1, CompletionMechanism.fromRaw(1))
        assertEquals(CompletionMechanism.VENDOR_IDLE, CompletionMechanism.fromRaw(2))
        assertEquals(CompletionMechanism.EPOS_JOB_ID, CompletionMechanism.fromRaw(3))
        assertEquals(CompletionMechanism.STAR_CHECKED_BLOCK, CompletionMechanism.fromRaw(4))
        assertEquals(CompletionMechanism.NONE, CompletionMechanism.fromRaw(5))
        // M13b (docs/wire-protocols.md section 2).
        assertEquals(CompletionMechanism.STAR_ETB, CompletionMechanism.fromRaw(6))
        assertEquals(CompletionMechanism.STAR_ESC_GS_ETX, CompletionMechanism.fromRaw(7))
        assertEquals(CompletionMechanism.UNRECOGNIZED, CompletionMechanism.fromRaw(8))
    }

    @Test
    fun `CutVariant fromRaw matches pd_cut_variant`() {
        assertEquals(CutVariant.PARTIAL, CutVariant.fromRaw(0))
        assertEquals(CutVariant.FULL, CutVariant.fromRaw(1))
        assertEquals(CutVariant.NONE, CutVariant.fromRaw(2))
        assertEquals(CutVariant.UNRECOGNIZED, CutVariant.fromRaw(3))
    }

    @Test
    fun `CodePage fromRaw mirrors pd h's non-contiguous ESC t n operand values`() {
        assertEquals(CodePage.PC437, CodePage.fromRaw(0))
        assertEquals(CodePage.PC850, CodePage.fromRaw(2))
        assertEquals(CodePage.WPC1252, CodePage.fromRaw(16))
        assertEquals(CodePage.PC852, CodePage.fromRaw(18))
        assertEquals(CodePage.PC858, CodePage.fromRaw(19))
        // 1 is a real gap in pd.h's ESC t n numbering, not PC850 -- proves this wrapper
        // is not silently treating the enum as contiguous/ordinal-based.
        assertEquals(CodePage.UNRECOGNIZED, CodePage.fromRaw(1))
        assertEquals(CodePage.UNRECOGNIZED, CodePage.fromRaw(17))
    }

    @Test
    fun `every enum's entries count matches its pd h _COUNT (minus UNRECOGNIZED)`() {
        // A cheap tripwire against a member being silently dropped or added without
        // updating fromRaw: pd.h's own _COUNT constants, restated here as the expected
        // sizes. Every number below is transcribed from a PD_*_COUNT in
        // capi/include/printerdriver/pd.h -- if one of them disagrees with the header,
        // this test is what is wrong, not the header.
        assertEquals(11, JobState.entries.size - 1)              // PD_JOB_STATE_COUNT
        assertEquals(6, ConfidenceLevel.entries.size - 1)        // PD_CONFIDENCE_COUNT
        assertEquals(13, DeviceEvent.entries.size - 1)           // PD_DEVICE_EVENT_COUNT
        assertEquals(11, FailureReason.entries.size - 1)         // PD_REASON_COUNT
        assertEquals(4, Cut.entries.size - 1)                    // PD_CUT_COUNT
        assertEquals(2, Preflight.entries.size - 1)              // PD_PREFLIGHT_COUNT
        assertEquals(3, Alignment.entries.size - 1)              // PD_ALIGN_COUNT
        assertEquals(5, CodePage.entries.size - 1)               // PD_CODE_PAGE_COUNT
        assertEquals(2, Binarization.entries.size - 1)           // PD_BINARIZATION_COUNT
        assertEquals(8, CompletionMechanism.entries.size - 1)    // PD_COMPLETION_COUNT
        assertEquals(3, CutVariant.entries.size - 1)             // PD_CUT_VARIANT_COUNT
        assertEquals(6, ConfidenceGrade.entries.size - 1)        // PD_GRADE_COUNT
        assertEquals(5, CompletionAuthority.entries.size - 1)    // PD_AUTHORITY_COUNT
        assertEquals(3, Provenance.entries.size - 1)             // PD_PROVENANCE_COUNT
        assertEquals(8, CommandLanguage.entries.size - 1)        // PD_LANGUAGE_COUNT
    }
}
