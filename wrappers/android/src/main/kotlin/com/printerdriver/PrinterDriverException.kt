package com.printerdriver

/**
 * Thrown for the C ABI's programmer-error-flavored null returns: a malformed payload,
 * handles from two different drivers crossed, an unknown capability profile id, a
 * storage directory that could not be created, and similar. These are the pd_* calls
 * pd.h documents as failing only on caller mistakes, not on routine/expected runtime
 * conditions -- contrast [PrinterDriver.findJob] and [Printer.forceReprint], which
 * return `null` instead, because "no job for this key" and "this job was reconstructed
 * from the journal and cannot be reprinted" are both things a correct caller can hit
 * in normal operation.
 *
 * The message is whatever `pd_last_error(driver)` held at the moment of failure where
 * a driver handle exists to ask (pd.h: valid until the next pd_* call on that driver,
 * which is why the JNI layer reads it immediately, before returning to Kotlin); for
 * pd_create failure there is no driver handle yet, so the message is a fixed string
 * (pd.h: "the reason is not retrievable, since there is no handle to hang it on").
 */
class PrinterDriverException(message: String) : RuntimeException(message)
