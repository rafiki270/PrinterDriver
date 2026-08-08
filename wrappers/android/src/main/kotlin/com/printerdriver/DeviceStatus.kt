package com.printerdriver

/**
 * Last known device state (pd_device_status) -- never a live query, so it cannot block
 * behind a print (use [Printer.refreshStatus] for that). [connected] and [observed]
 * are plain booleans (the ABI never reports them as PD_UNKNOWN); every other field is
 * genuinely tri-state and maps to a nullable [Boolean] here: `null` means
 * PD_UNKNOWN -- "a snapshot that has never heard from the device says so rather than
 * reporting healthy" (pd.h) -- `false`/`true` mean PD_FALSE/PD_TRUE. This is the same
 * mapping pd_capi.cpp's own `tristate()` helper performs in the opposite direction,
 * from `std::optional<bool>` to the C int32 tri-state; this wrapper just reverses it.
 */
data class DeviceStatus(
    val connected: Boolean,
    /** `false` until a status frame has actually been decoded at least once. */
    val observed: Boolean,
    val online: Boolean?,
    val coverOpen: Boolean?,
    val paperOut: Boolean?,
    val paperNearEnd: Boolean?,
    val cutterError: Boolean?,
    val unrecoverableError: Boolean?,
    val recoverableError: Boolean?
) {
    internal companion object {
        /**
         * Builds a [DeviceStatus] from the 9-element int array NativeBridge.printerStatus
         * / printerRefreshStatus return, in the exact field order pd_device_status
         * declares them (see printerdriver_jni.cpp's packing of the same struct).
         */
        fun fromRaw(raw: IntArray): DeviceStatus {
            require(raw.size == 9) { "expected 9 pd_device_status fields, got ${raw.size}" }
            return DeviceStatus(
                connected = raw[0] != 0,
                observed = raw[1] != 0,
                online = triState(raw[2]),
                coverOpen = triState(raw[3]),
                paperOut = triState(raw[4]),
                paperNearEnd = triState(raw[5]),
                cutterError = triState(raw[6]),
                unrecoverableError = triState(raw[7]),
                recoverableError = triState(raw[8])
            )
        }

        // PD_UNKNOWN(-1) / PD_FALSE(0) / PD_TRUE(1), defensively treating anything
        // else as unknown rather than guessing a polarity.
        private fun triState(raw: Int): Boolean? = when (raw) {
            1 -> true
            0 -> false
            else -> null
        }
    }
}
