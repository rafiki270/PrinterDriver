package com.printerdriver

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Pure-Kotlin tests for the pd_device_status 9-int packing -> [DeviceStatus] mapping,
 * in particular the PD_UNKNOWN(-1)/PD_FALSE(0)/PD_TRUE(1) tri-state fields decoding to
 * Boolean?. No native calls; see EnumMappingTest's header comment for why that matters
 * here.
 *
 * UNVERIFIED: written but never executed -- see README.md "Verification status".
 */
class DeviceStatusTest {

    @Test
    fun `connected and observed are plain booleans, never tri-state`() {
        val status = DeviceStatus.fromRaw(intArrayOf(1, 0, -1, -1, -1, -1, -1, -1, -1))
        assertTrue(status.connected)
        assertFalse(status.observed)
    }

    @Test
    fun `a never-observed snapshot reports every tri-state field as null, not false`() {
        // pd.h: "a snapshot that has never heard from the device says so rather than
        // reporting healthy" -- the case this test pins down.
        val status = DeviceStatus.fromRaw(intArrayOf(1, 0, -1, -1, -1, -1, -1, -1, -1))
        assertNull(status.online)
        assertNull(status.coverOpen)
        assertNull(status.paperOut)
        assertNull(status.paperNearEnd)
        assertNull(status.cutterError)
        assertNull(status.unrecoverableError)
        assertNull(status.recoverableError)
    }

    @Test
    fun `PD_TRUE and PD_FALSE decode to true and false in field order`() {
        val status = DeviceStatus.fromRaw(intArrayOf(1, 1, 1, 0, 1, 0, 1, 0, 1))
        assertEquals(true, status.online)
        assertEquals(false, status.coverOpen)
        assertEquals(true, status.paperOut)
        assertEquals(false, status.paperNearEnd)
        assertEquals(true, status.cutterError)
        assertEquals(false, status.unrecoverableError)
        assertEquals(true, status.recoverableError)
    }

    @Test(expected = IllegalArgumentException::class)
    fun `fromRaw rejects anything other than exactly 9 fields`() {
        DeviceStatus.fromRaw(intArrayOf(1, 0, -1))
    }
}
