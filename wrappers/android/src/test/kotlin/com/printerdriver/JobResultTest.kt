package com.printerdriver

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Pure-Kotlin tests for the pd_job_result -> sealed [JobResult] mapping -- the
 * tri-state boundary docs/api.md §1.4 and §4 center the whole SDK design on. No
 * native calls; see EnumMappingTest's header comment for why that matters here.
 *
 * UNVERIFIED: written but never executed -- see README.md "Verification status".
 */
class JobResultTest {

    @Test
    fun `outcome 0 maps to Done, carrying confidence`() {
        val result = JobResult.fromRaw(
            outcome = 0, confidence = 4, reason = 0,
            grade = 0, authority = 0, method = "GS(H) fn48"
        )
        assertTrue(result is JobResult.Done)
        val done = result as JobResult.Done
        assertEquals(ConfidenceLevel.CUT_FAULT_FREE, done.confidence)
        // Never a bare success: what class of evidence, who said so, and which command.
        assertEquals(ConfidenceGrade.A_JOB_LEVEL_CONFIRMATION, done.grade)
        assertEquals("A", done.grade.letter)
        assertEquals(CompletionAuthority.PHYSICAL_PRINTER, done.authority)
        assertEquals("GS(H) fn48", done.method)
    }

    @Test
    fun `an unrecognized grade or authority degrades rather than claiming evidence`() {
        val result = JobResult.fromRaw(
            outcome = 0, confidence = 0, reason = 0,
            grade = 99, authority = 99, method = "none"
        )
        val done = result as JobResult.Done
        assertEquals(ConfidenceGrade.UNRECOGNIZED, done.grade)
        assertEquals("?", done.grade.letter)
        assertEquals(CompletionAuthority.UNRECOGNIZED, done.authority)
    }

    @Test
    fun `outcome 1 maps to Failed, carrying both reason and confidence`() {
        // pd.h's pd_job_result struct comment is explicit that confidence is carried
        // on Failed too ("what an operator needs to decide about a reprint"), not only
        // on Done -- this is the case that pins that down.
        val result = JobResult.fromRaw(outcome = 1, confidence = 1, reason = 3, grade = 4, authority = 4, method = "none")
        assertTrue(result is JobResult.Failed)
        val failed = result as JobResult.Failed
        assertEquals(FailureReason.PREFLIGHT_PAPER_OUT, failed.reason)
        assertEquals(ConfidenceLevel.PRINTER_HEALTHY, failed.confidence)
    }

    @Test
    fun `outcome 2 maps to Unknown, carrying confidence, never collapsed into Done or Failed`() {
        val result = JobResult.fromRaw(outcome = 2, confidence = 2, reason = 0, grade = 4, authority = 4, method = "none")
        assertTrue(result is JobResult.Unknown)
        assertEquals(ConfidenceLevel.PRINT_CONFIRMED, (result as JobResult.Unknown).confidence)
    }

    @Test
    fun `an unrecognized outcome value degrades to Unknown, the never-assume bucket`() {
        // Same "closed enum, explicit unrecognized handling" contract as Enums.kt, but
        // enforced at the sealed-class boundary instead of an enum's: an outcome this
        // wrapper does not recognize must never be silently reported as Done or Failed.
        val result = JobResult.fromRaw(outcome = 99, confidence = 0, reason = 0, grade = 4, authority = 4, method = "none")
        assertTrue(result is JobResult.Unknown)
    }

    @Test
    fun `Failed reason maps through FailureReason fromRaw, including its UNRECOGNIZED fallback`() {
        val result = JobResult.fromRaw(outcome = 1, confidence = 0, reason = 12345, grade = 4, authority = 4, method = "none")
        assertTrue(result is JobResult.Failed)
        assertEquals(FailureReason.UNRECOGNIZED, (result as JobResult.Failed).reason)
    }
}
