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
        assertEquals(DeviceEvent.UNRECOGNIZED, DeviceEvent.fromRaw(12))
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
        assertEquals(CompletionMechanism.NONE, CompletionMechanism.fromRaw(2))
        assertEquals(CompletionMechanism.UNRECOGNIZED, CompletionMechanism.fromRaw(3))
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
        // sizes (JobState 11, ConfidenceLevel 6, DeviceEvent 12, FailureReason 11,
        // Cut 4, Preflight 2, Alignment 3, CodePage 5, Binarization 2,
        // CompletionMechanism 3, CutVariant 3).
        assertEquals(11, JobState.entries.size - 1)
        assertEquals(6, ConfidenceLevel.entries.size - 1)
        assertEquals(12, DeviceEvent.entries.size - 1)
        assertEquals(11, FailureReason.entries.size - 1)
        assertEquals(4, Cut.entries.size - 1)
        assertEquals(2, Preflight.entries.size - 1)
        assertEquals(3, Alignment.entries.size - 1)
        assertEquals(5, CodePage.entries.size - 1)
        assertEquals(2, Binarization.entries.size - 1)
        assertEquals(3, CompletionMechanism.entries.size - 1)
        assertEquals(3, CutVariant.entries.size - 1)
    }
}
