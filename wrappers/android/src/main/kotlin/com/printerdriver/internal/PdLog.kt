package com.printerdriver.internal

import android.util.Log

/**
 * One log tag for the whole wrapper, so every "this shouldn't happen but let's not
 * crash" path (unrecognized raw enum values from a version-skewed native library,
 * dropped callbacks during JVM shutdown) is greppable in logcat under one name.
 */
internal object PdLog {
    private const val TAG = "PrinterDriver"

    fun w(message: String) {
        Log.w(TAG, message)
    }
}
