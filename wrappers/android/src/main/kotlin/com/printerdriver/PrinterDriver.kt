package com.printerdriver

import com.printerdriver.internal.NativeBridge
import com.printerdriver.internal.NativeLogCallback
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * The service (docs/api.md §2): owns connections, queues, the persistent job store.
 * One [PrinterDriver] wraps one pd_driver -- construct with [create], release with
 * [close] (or `use { ... }`, since this implements [AutoCloseable]).
 *
 * Construction can fail (pd_create returns NULL only when its storage directory could
 * not be created), which a Kotlin constructor cannot express -- hence the
 * `create`-then-throw factory instead of a public constructor.
 */
class PrinterDriver private constructor(internal val handle: Long) : AutoCloseable {

    private val closedFlag = AtomicBoolean(false)

    // Backs Printer.send's closure-ergonomics sugar (docs/api.md §12) -- Dispatchers.Main
    // so onProgress/onResult land on the main thread by default, the conventional
    // choice for callback-style Android APIs; see Printer.send's KDoc. A SupervisorJob
    // so one submission's failure/cancellation cannot cancel unrelated ones sharing
    // this scope.
    private val closureScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    /** Why the last call on this driver returned NULL/failed. Never null; "" when the
     *  last call succeeded (pd_last_error's own contract, unchanged here). Only
     *  meaningful immediately after a failure -- pd.h: "valid until the next pd_* call
     *  on that same driver." */
    val lastError: String
        get() {
            checkOpen()
            return NativeBridge.driverLastError(handle)
        }

    /** The capability-profile ids [TcpPrinterConfig.profileId] accepts (pd_profile_ids) --
     *  enumerated from the native layer rather than hardcoded here, so this list can
     *  never drift out of sync with what the linked native library actually supports. */
    fun profileIds(): List<String> {
        checkOpen()
        return NativeBridge.profileIds().toList()
    }

    /** Fleet-style: a stable printerId (docs/api.md §2). Throws [PrinterDriverException]
     *  on failure (empty host, unknown [TcpPrinterConfig.profileId]) -- both are
     *  configuration mistakes, not routine runtime conditions. */
    fun addPrinterTcp(config: TcpPrinterConfig): Printer {
        checkOpen()
        val printerHandle = NativeBridge.addPrinterTcp(
            handle, config.printerId, config.host, config.port, config.widthDots,
            config.profileId, config.connectTimeoutMs
        )
        if (printerHandle == 0L) {
            throw PrinterDriverException("pd_add_printer_tcp failed: $lastError")
        }
        return Printer(this, printerHandle)
    }

    /** Looks up any job this driver knows about, including ones reloaded from the
     *  journal after a restart (docs/api.md §2). `null` when [key] is unknown -- a
     *  routine, expected outcome, not an error. */
    /**
     * Paper to job (docs/api.md §14): resolves the four-character `V:` code printed on
     * a receipt.
     *
     * Accepts either of a job's identifiers, the print fence's or the cut fence's, and
     * answers most-recent-first -- the sequence wraps, and the receipt somebody is
     * holding is far more likely to be the recent one. Includes jobs reconstructed from
     * the journal, so a receipt printed before the last restart still resolves. `null`
     * when no job on this driver ever carried that token.
     */
    fun jobByToken(token: String): PrintJob? {
        checkOpen()
        val jobHandle = NativeBridge.jobByToken(handle, token)
        return if (jobHandle == 0L) null else PrintJob(this, jobHandle)
    }

    /**
     * The two characters every identifier this driver issues starts with: which driver
     * instance owns an echo, and therefore which instance printed a given receipt.
     * Persisted in the storage directory, so it survives a restart. A token that does
     * not start with this came from somewhere else -- the case
     * [DeviceEvent.FOREIGN_WRITER_DETECTED] reports.
     */
    val instanceNonce: String get() = NativeBridge.instanceNonce(handle)

    fun findJob(key: String): PrintJob? {
        checkOpen()
        val jobHandle = NativeBridge.findJob(handle, key)
        return if (jobHandle == 0L) null else PrintJob(this, jobHandle)
    }

    /**
     * Stops every printer worker, waits for in-flight jobs to reach a terminal state,
     * and frees every [Printer]/[PrintJob] handle this driver ever returned
     * (pd_destroy's contract, unchanged here). Idempotent -- a second call is a no-op.
     *
     * Cancels the closure-sugar scope first, then calls pd_destroy. Because
     * pd_job_await/pd_subscribe_job callbacks run on background threads that are not
     * cooperatively cancellable mid-native-call (see [PrintJob.result]'s KDoc), a
     * [Printer.send] callback that is already inside its native wait when [close] runs
     * may still fire once the shutdown-triggered terminal state comes through, even
     * though the scope has been asked to cancel. Callers that need a hard guarantee
     * that no callback fires after [close] returns should await outstanding results
     * first.
     */
    override fun close() {
        if (closedFlag.compareAndSet(false, true)) {
            closureScope.cancel()
            NativeBridge.driverDestroy(handle)
        }
    }

    internal fun checkOpen() {
        check(!closedFlag.get()) { "PrinterDriver is closed" }
    }

    internal fun launchClosureSugar(block: suspend CoroutineScope.() -> Unit) {
        checkOpen()
        closureScope.launch(block = block)
    }

    companion object {
        /** @throws PrinterDriverException if pd_create fails -- pd.h documents this as
         *  only possible when [PrinterDriverConfig.storageDirectory] could not be
         *  created; there is no driver handle yet at that point for pd_last_error to
         *  report against, so the message here is fixed rather than ABI-sourced. */
        fun create(config: PrinterDriverConfig = PrinterDriverConfig()): PrinterDriver {
            val nativeLogCallback = config.onLog?.let { callback -> NativeLogCallback { message -> callback(message) } }
            val handle = NativeBridge.driverCreate(config.storageDirectory, config.fsyncDisabled, nativeLogCallback)
            if (handle == 0L) {
                throw PrinterDriverException(
                    "pd_create returned null; the only documented cause is a storage " +
                        "directory that could not be created (see pd.h's pd_create doc)"
                )
            }
            return PrinterDriver(handle)
        }
    }
}
