package com.printerdriver.internal

/**
 * The entire JNI surface, in one place. Every function here is a thin, mechanical
 * crossing into src/main/cpp/printerdriver_jni.cpp -- no decisions get made on this
 * side of the boundary, only marshaling (see Enums.kt's file-level note and
 * Payload.kt's header comment for what "mechanical" means here). Public Kotlin API
 * types (PrinterDriver, Printer, PrintJob, the sealed Payload/JobResult, the mirrored
 * enums) are the only supported way to reach these; app code should never call
 * NativeBridge directly.
 *
 * Handles: pd_driver*/pd_printer*/pd_job* cross the JNI boundary as `Long` (a raw
 * pointer value reinterpret_cast to jlong on the native side -- see AsDriverHandle/
 * AsPrinter/AsJob in printerdriver_jni.cpp). 0L is the failure/not-found sentinel
 * everywhere a function can legitimately return "no handle" (mirroring pd.h's own
 * NULL-on-failure convention); it is never a valid handle.
 *
 * Visibility: this object is `internal`, but its individual members are deliberately
 * left at default (public) visibility -- see NativeCallbacks.kt's header comment for
 * why (Kotlin's `internal`-function name mangling would break the
 * Java_com_printerdriver_internal_NativeBridge_<name> symbol match). `@JvmStatic` is
 * required on every entry so each compiles to a real `static native` JVM method
 * (JNI: `(JNIEnv*, jclass, ...)`) rather than an instance method requiring the
 * object's singleton `this` (JNI: `(JNIEnv*, jobject, ...)`); the native
 * implementations are written for the static form.
 */
internal object NativeBridge {
    init {
        System.loadLibrary("printerdriver_jni")
    }

    // --- Driver (pd_create / pd_destroy / pd_last_error / pd_profile_ids) ----------

    /** Returns 0L on failure (see PrinterDriverException's docs for why pd_create
     *  alone cannot carry a message). [logCallback] may be null. */
    @JvmStatic external fun driverCreate(storageDirectory: String?, fsyncDisabled: Boolean, logCallback: NativeLogCallback?): Long

    /** Idempotent no-op on an already-destroyed/0L handle, matching pd_destroy(NULL). */
    @JvmStatic external fun driverDestroy(driverHandle: Long)

    @JvmStatic external fun driverLastError(driverHandle: Long): String

    @JvmStatic external fun profileIds(): Array<String>

    // --- Printers (pd_add_printer_tcp and friends) -----------------------------------

    /** Returns 0L on failure (bad host, unknown profile id) -- check driverLastError. */
    @JvmStatic external fun addPrinterTcp(
        driverHandle: Long,
        printerId: String?,
        host: String,
        port: Int,
        widthDots: Int,
        profileId: String?,
        connectTimeoutMs: Int
    ): Long

    /**
     * pd_add_printer_custom: a printer reached over a link this wrapper owns -- for now
     * only Bluetooth Classic SPP (see com.printerdriver.BluetoothSppTransport). Returns
     * 0L on failure (unknown [profileId], or a vtable the core refused) -- check
     * [driverLastError].
     *
     * [callback] and the native context wrapping it are retained for the life of the
     * driver, as pd.h requires ("`ctx` must remain alive until pd_destroy"). [description]
     * is what the printer id and the diagnostics derive from, e.g.
     * "bt-spp:00:11:22:33:44:55"; "" becomes "custom", which is fine for one printer and
     * ambiguous for two.
     *
     * The core may call [callback]`.connect()` before this function's return value has
     * reached Kotlin -- pd_add_printer_custom starts the printer's worker thread, and
     * that thread queues a capability probe immediately. An implementation therefore
     * cannot assume it has been told its own printer handle by the time it is first
     * connected; see BluetoothSppTransport's handle latch.
     */
    @JvmStatic external fun addPrinterCustom(
        driverHandle: Long,
        callback: NativeTransportCallback,
        description: String,
        profileId: String?,
        widthDots: Int
    ): Long

    /**
     * pd_transport_feed_bytes: deliver [length] bytes of [data] the link received.
     *
     * Safe from any thread, including while a [NativeTransportCallback.write] is in
     * flight -- that is the normal case, a status answer arriving while the next chunk
     * goes out. Must NOT be called from inside connect/write/close.
     *
     * Returns `false` when nothing was listening (the core has not connected yet, or has
     * already closed). pd.h calls that information rather than an error: bytes arriving
     * with no reader are dropped exactly as they would be on a socket nobody is reading.
     *
     * Takes [driverHandle] as well as [printerHandle] purely so the native side can
     * serialise this against pd_destroy -- pd_destroy frees every pd_printer, and a
     * reader thread that outlived it would otherwise feed bytes through a dangling
     * pointer.
     */
    @JvmStatic external fun transportFeedBytes(
        driverHandle: Long,
        printerHandle: Long,
        data: ByteArray,
        length: Int
    ): Boolean

    /** pd_transport_link_dropped: the link went away for a reason other than an explicit
     *  close -- out of range, the OS tore the channel down, pairing was revoked. Surfaces
     *  as [com.printerdriver.DeviceEvent.CONNECTION_LOST] and fails any job waiting on a
     *  fence instead of leaving it to time out. Returns `false` when there was no live
     *  transport to notify. Same lifetime guard as [transportFeedBytes]. */
    @JvmStatic external fun transportLinkDropped(
        driverHandle: Long,
        printerHandle: Long,
        message: String
    ): Boolean

    @JvmStatic external fun printerId(printerHandle: Long): String
    @JvmStatic external fun printerWidthDots(printerHandle: Long): Int
    @JvmStatic external fun printerCompletionMechanism(printerHandle: Long): Int

    /** Returns the 9 pd_device_status fields packed as
     *  [connected, observed, online, coverOpen, paperOut, paperNearEnd, cutterError,
     *  unrecoverableError, recoverableError] -- see DeviceStatus.fromRaw. Never blocks
     *  (pd_printer_status is a snapshot read). */
    @JvmStatic external fun printerStatus(driverHandle: Long, printerHandle: Long): IntArray

    /** Same packing as [printerStatus]. Blocks behind any active job -- call from a
     *  background dispatcher (Printer.refreshStatus already does). */
    @JvmStatic external fun printerRefreshStatus(driverHandle: Long, printerHandle: Long, timeoutMs: Int): IntArray

    @JvmStatic external fun openCashDrawer(driverHandle: Long, printerHandle: Long)

    // --- M14: cash drawer (docs/cash-drawer.md) -------------------------------------

    /** Returns the 18 pd_drawer_capabilities fields packed as
     *  [present, standard, voltage, maxCurrentMa, channelCount, sensorPin, method,
     *  defaultPulseMs, maxPulseMs, cooldownMs, canKickDuringPrint, statusAvailable,
     *  statusMethod, sharedBetweenDrawers, sharedWithBuzzer, electricalProvenance,
     *  commandsProvenance, kickable] -- see DrawerCapabilities.fromRaw. Never blocks. */
    @JvmStatic external fun drawerCapabilities(printerHandle: Long): IntArray

    /** Runs the verified opening sequence and returns the 5 pd_drawer_result fields as
     *  [state, previousState, channel, pulseMs, elapsedMs] -- see DrawerResult.fromRaw.
     *  Blocks until the sequence reaches a verdict; call from a background dispatcher
     *  (Printer.openDrawer already does). */
    @JvmStatic external fun drawerOpen(
        driverHandle: Long,
        printerHandle: Long,
        channel: Int,
        pulseMs: Int
    ): IntArray

    /** Reads the switch without firing anything. Returns the 5 pd_drawer_reading fields
     *  as [available, answered, pinHigh, needsCalibration, state], with pinHigh carrying
     *  the ABI's tri-state (-1 = PD_UNKNOWN). Blocks. */
    @JvmStatic external fun drawerReadSensor(
        driverHandle: Long,
        printerHandle: Long,
        timeoutMs: Int
    ): IntArray

    /** Records and persists which sense level means "open" for the attached drawer.
     *  Returns 1 when persisted, 0 when it applies to this process only. */
    @JvmStatic external fun drawerCalibratePolarity(
        driverHandle: Long,
        printerHandle: Long,
        highMeansOpen: Boolean
    ): Int

    @JvmStatic external fun drawerPolarityCalibrated(driverHandle: Long, printerHandle: Long): Int

    @JvmStatic external fun drawerHighMeansOpen(driverHandle: Long, printerHandle: Long): Int

    /** Blocks until the printer's queue is empty and its active job is terminal. */
    @JvmStatic external fun printerDrain(driverHandle: Long, printerHandle: Long)

    /** Registers [callback] for the life of the driver -- there is no
     *  pd_unsubscribe_device in the C ABI. See README.md "Threading contract". */
    @JvmStatic external fun subscribeDevice(driverHandle: Long, printerHandle: Long, callback: NativeDeviceEventCallback)

    // --- Jobs: submit (pd_print, one native entry point per payload tier) -----------
    //
    // Three entry points instead of one generic `print(payload: Any)` so the native
    // side never has to reflect Kotlin object fields back out through JNI -- callers
    // (Printer.print) flatten the sealed Payload into primitives/arrays first. Options
    // are passed as their five primitive fields rather than a JobOptions object for
    // the same reason.

    /** Returns 0L on failure (malformed payload / handle mismatch) -- check
     *  driverLastError. [strideBytes] 0 means tightly packed; [threshold] 0 and
     *  [maxRowsPerBand] 0 mean "use the core's default", matching pd_raster_rgba8. */
    @JvmStatic external fun printRaster(
        driverHandle: Long,
        printerHandle: Long,
        pixels: ByteArray,
        width: Int,
        height: Int,
        strideBytes: Int,
        binarization: Int,
        threshold: Int,
        maxRowsPerBand: Int,
        key: String?,
        cut: Int,
        openDrawer: Boolean,
        preflight: Int,
        timeoutMs: Int,
        topFeedDots: Int,
        bottomFeedDots: Int,
        suppressVerificationId: Boolean
    ): Long

    /** [opKinds]/[opTexts]/[opValues] are parallel arrays, one entry per pd_op (raw
     *  pd_op_kind, nullable text, value) -- see Printer.print's flattening of
     *  List<DocumentOp>. Returns 0L on failure. */
    @JvmStatic external fun printDocument(
        driverHandle: Long,
        printerHandle: Long,
        opKinds: IntArray,
        opTexts: Array<String?>,
        opValues: IntArray,
        codePage: Int,
        key: String?,
        cut: Int,
        openDrawer: Boolean,
        preflight: Int,
        timeoutMs: Int,
        topFeedDots: Int,
        bottomFeedDots: Int,
        suppressVerificationId: Boolean
    ): Long

    /** Returns 0L on failure. */
    @JvmStatic external fun printRaw(
        driverHandle: Long,
        printerHandle: Long,
        bytes: ByteArray,
        key: String?,
        cut: Int,
        openDrawer: Boolean,
        preflight: Int,
        timeoutMs: Int,
        topFeedDots: Int,
        bottomFeedDots: Int,
        suppressVerificationId: Boolean
    ): Long

    /** Returns 0L when [key] is unknown, or when its job was reconstructed from the
     *  journal (pd.h: "those records carry what happened to a job, never what it
     *  contained") -- both routine, both handled by Printer.forceReprint returning
     *  `null` rather than throwing. */
    @JvmStatic external fun forceReprint(
        driverHandle: Long,
        printerHandle: Long,
        key: String,
        cut: Int,
        openDrawer: Boolean,
        preflight: Int,
        timeoutMs: Int,
        topFeedDots: Int,
        bottomFeedDots: Int,
        suppressVerificationId: Boolean,
        suppressBanner: Boolean
    ): Long

    /** Returns 0L when [key] is unknown. */
    @JvmStatic external fun findJob(driverHandle: Long, key: String): Long

    /** Paper to job (docs/api.md §14): resolves either of a job's four-character
     *  verification identifiers, most-recent-first, including jobs reloaded from the
     *  journal. Returns 0L when no job on this driver ever carried that token. */
    @JvmStatic external fun jobByToken(driverHandle: Long, token: String): Long

    /** The two characters every identifier this driver issues starts with: which driver
     *  instance owns an echo. Persisted in the storage directory, so it survives a
     *  restart. */
    @JvmStatic external fun instanceNonce(driverHandle: Long): String

    // --- Jobs: accessors (none of these take a driver handle -- pd.h doesn't either) -

    @JvmStatic external fun jobId(jobHandle: Long): String
    @JvmStatic external fun jobKey(jobHandle: Long): String
    @JvmStatic external fun jobAttempt(jobHandle: Long): Int
    @JvmStatic external fun jobCurrentState(jobHandle: Long): Int
    @JvmStatic external fun jobConfidence(jobHandle: Long): Int
    @JvmStatic external fun jobIsTerminal(jobHandle: Long): Boolean

    /** The identifier the ticket carries as `V:`, or "" until the job reaches a worker
     *  and for good on a printer whose fence is not GS(H) -- the identifier *is* the
     *  wire token (docs/api.md §14). */
    @JvmStatic external fun jobPrintToken(jobHandle: Long): String

    /** The identifier the job's cut fence carried. Same rules as [jobPrintToken]. */
    @JvmStatic external fun jobCutToken(jobHandle: Long): String

    /** Replays every event recorded so far synchronously on the calling thread, then
     *  streams the rest on the printer's worker thread (pd_subscribe_job's documented
     *  contract, unchanged here) -- see README.md "Threading contract". Registers
     *  [callback] for the life of the driver; there is no pd_unsubscribe_job. */
    @JvmStatic external fun subscribeJob(driverHandle: Long, jobHandle: Long, callback: NativeJobEventCallback)

    /** Returns null on timeout (mirrors pd_job_await returning 0 / leaving `out`
     *  untouched); otherwise the five integer pd_job_result fields packed as
     *  [outcome, confidence, reason, grade, authority] -- see JobResult.fromRaw. The
     *  sixth field, `method`, is a string and comes from [jobMethod]. [timeoutMs] 0
     *  waits indefinitely; PrintJob.result() never passes 0 directly (it polls with a
     *  bounded interval instead, for coroutine-cancellation responsiveness -- see
     *  PrintJob.kt). */
    @JvmStatic external fun jobAwait(driverHandle: Long, jobHandle: Long, timeoutMs: Int): IntArray?

    /** `pd_job_result.method` for a job [jobAwait] has already settled: the command
     *  behind the claim, e.g. "GS(H) fn48". A separate call because the value is a
     *  string and the rest of the struct is an int array; it re-reads the same terminal
     *  result, which pd_job_await answers immediately once a job is terminal. Returns
     *  "" if the job is somehow still running. */
    @JvmStatic external fun jobMethod(driverHandle: Long, jobHandle: Long): String
}
