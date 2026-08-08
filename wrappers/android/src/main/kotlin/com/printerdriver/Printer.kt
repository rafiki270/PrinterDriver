package com.printerdriver

import com.printerdriver.internal.NativeBridge
import com.printerdriver.internal.NativeDeviceEventCallback
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

// pd_op_kind raw values (pd.h) -- internal-only, used to flatten List<DocumentOp> into
// the parallel primitive arrays NativeBridge.printDocument crosses into JNI with.
private object PdOpKind {
    const val TEXT = 0
    const val LINE = 1
    const val ALIGN = 2
    const val BOLD = 3
    const val FEED = 4
}

/**
 * A handle to one configured device (docs/api.md §2): transport + width + capability
 * profile. Constructed only via [PrinterDriver.addPrinterTcp] -- there is no public
 * constructor, matching pd.h (a pd_printer is only ever produced by pd_add_printer_tcp
 * and owned by its driver).
 */
class Printer internal constructor(
    private val driver: PrinterDriver,
    internal val handle: Long
) {
    val id: String get() { driver.checkOpen(); return NativeBridge.printerId(handle) }
    val widthDots: Int get() { driver.checkOpen(); return NativeBridge.printerWidthDots(handle) }

    val completionMechanism: CompletionMechanism
        get() { driver.checkOpen(); return CompletionMechanism.fromRaw(NativeBridge.printerCompletionMechanism(handle)) }

    /** Snapshot: online, paper, cover, errors. Never blocks (pd_printer_status is a
     *  cached-status read, not a live query). */
    fun status(): DeviceStatus {
        driver.checkOpen()
        return DeviceStatus.fromRaw(NativeBridge.printerStatus(driver.handle, handle))
    }

    /** Queues a status round trip behind any active job and waits for the answer.
     *  Blocks (on [Dispatchers.IO] here) -- prefer [status] unless you specifically
     *  need a fresh read. [timeoutMs] 0 -> the core's default (2000ms). */
    suspend fun refreshStatus(timeoutMs: Int = 0): DeviceStatus = withContext(Dispatchers.IO) {
        driver.checkOpen()
        DeviceStatus.fromRaw(NativeBridge.printerRefreshStatus(driver.handle, handle, timeoutMs))
    }

    fun openCashDrawer() {
        driver.checkOpen()
        NativeBridge.openCashDrawer(driver.handle, handle)
    }

    /** Blocks until this printer's queue is empty and its active job is terminal. */
    suspend fun drain() = withContext(Dispatchers.IO) {
        driver.checkOpen()
        NativeBridge.printerDrain(driver.handle, handle)
    }

    /** Replaces availability ping-polling (docs/api.md §4): online/offline,
     *  cover, paper, cutter and connection transitions for this printer. Runs for the
     *  life of the driver; there is no terminal event and no pd_unsubscribe_device --
     *  see README.md "Threading contract". */
    val events: Flow<DeviceEvent> = callbackFlow {
        driver.checkOpen()
        val callback = NativeDeviceEventCallback { raw -> trySend(DeviceEvent.fromRaw(raw)) }
        NativeBridge.subscribeDevice(driver.handle, handle, callback)
        awaitClose { /* see events' KDoc and README.md "Threading contract". */ }
    }

    /**
     * The one submit call (docs/api.md §2). Suspends because the underlying pd_print
     * can touch the durable job store (a journal write, fsync'd unless
     * [PrinterDriverConfig.fsyncDisabled]) before returning -- this wrapper always hops
     * to [Dispatchers.IO] for that, the same way the Swift sketch in docs/api.md §5
     * marks its equivalent `try await`.
     *
     * Throws [PrinterDriverException] on failure (malformed payload, or a handle from
     * a different [PrinterDriver]) -- see that class's docs for why this throws rather
     * than returning null, unlike [forceReprint].
     */
    suspend fun print(payload: Payload, options: JobOptions = JobOptions()): PrintJob = withContext(Dispatchers.IO) {
        driver.checkOpen()
        val jobHandle = when (payload) {
            is Payload.Raster -> NativeBridge.printRaster(
                driver.handle, handle,
                payload.pixels, payload.width, payload.height, payload.strideBytes,
                payload.binarization.raw, payload.threshold, payload.maxRowsPerBand,
                options.key, options.cut.raw, options.openDrawer, options.preflight.raw, options.timeoutMs
            )

            is Payload.Document -> {
                val ops = payload.ops
                val kinds = IntArray(ops.size)
                val texts = arrayOfNulls<String>(ops.size)
                val values = IntArray(ops.size)
                ops.forEachIndexed { index, op ->
                    when (op) {
                        is DocumentOp.Text -> {
                            kinds[index] = PdOpKind.TEXT
                            texts[index] = op.text
                        }
                        is DocumentOp.Line -> {
                            kinds[index] = PdOpKind.LINE
                            texts[index] = op.text
                        }
                        is DocumentOp.Align -> {
                            kinds[index] = PdOpKind.ALIGN
                            values[index] = op.alignment.raw
                        }
                        is DocumentOp.Bold -> {
                            kinds[index] = PdOpKind.BOLD
                            values[index] = if (op.enabled) 1 else 0
                        }
                        is DocumentOp.Feed -> {
                            kinds[index] = PdOpKind.FEED
                            values[index] = op.lines
                        }
                    }
                }
                NativeBridge.printDocument(
                    driver.handle, handle, kinds, texts, values, payload.codePage.raw,
                    options.key, options.cut.raw, options.openDrawer, options.preflight.raw, options.timeoutMs
                )
            }

            is Payload.Raw -> NativeBridge.printRaw(
                driver.handle, handle, payload.bytes,
                options.key, options.cut.raw, options.openDrawer, options.preflight.raw, options.timeoutMs
            )
        }
        if (jobHandle == 0L) {
            throw PrinterDriverException("pd_print failed: ${driver.lastError}")
        }
        PrintJob(driver, jobHandle)
    }

    /**
     * Deliberate duplicate of an already-submitted [key]: reuses the original payload
     * and prepends the reprint banner and attempt counter (docs/api.md §3). Returns
     * `null` when [key] is unknown, or when its job was reconstructed from the journal
     * after a restart (pd.h: those records "carry what happened to a job, never what
     * it contained") -- both are routine conditions a caller needs to branch on, not
     * programmer errors, which is why this returns `null` rather than throwing (unlike
     * [print]). Check [PrinterDriver.lastError] to tell the two apart if needed.
     */
    suspend fun forceReprint(key: String, options: JobOptions = JobOptions()): PrintJob? = withContext(Dispatchers.IO) {
        driver.checkOpen()
        val jobHandle = NativeBridge.forceReprint(
            driver.handle, handle, key,
            options.cut.raw, options.openDrawer, options.preflight.raw, options.timeoutMs
        )
        if (jobHandle == 0L) null else PrintJob(driver, jobHandle)
    }

    /**
     * Closure-ergonomics sugar (docs/api.md §12): a trailing lambda for the terminal
     * result plus an optional [onProgress] callback, for call sites that would rather
     * not use coroutines directly. Both are sugar over exactly the same [print] /
     * [PrintJob.events] / [PrintJob.result] path used everywhere else in this file --
     * "no separate code path" (api.md §12) -- just launched on [PrinterDriver]'s
     * internal scope instead of the caller's.
     *
     * [onResult] fires exactly once, always -- that guarantee comes directly from
     * [PrintJob.result]'s suspend contract (a suspend function returns exactly once),
     * and holds equally for a job obtained here or via [PrinterDriver.findJob] after a
     * crash/restart, since both wrap the same pd_job* accessors.
     *
     * Callbacks land on [kotlinx.coroutines.Dispatchers.Main] (see PrinterDriver's
     * internal scope) -- the conventional default for callback-style Android APIs; hop
     * elsewhere yourself inside [onProgress]/[onResult] if you need to.
     */
    fun send(
        payload: Payload,
        options: JobOptions = JobOptions(),
        onProgress: ((JobEvent) -> Unit)? = null,
        onResult: (JobResult) -> Unit
    ) {
        driver.launchClosureSugar {
            val job = print(payload, options)
            val progressJob = onProgress?.let { callback -> launch { job.events.collect { callback(it) } } }
            val result = job.result()
            progressJob?.cancel()
            onResult(result)
        }
    }
}
