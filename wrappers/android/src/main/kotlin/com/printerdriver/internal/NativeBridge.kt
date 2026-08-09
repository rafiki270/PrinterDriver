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
    // --- M15: self-test, auto-detection and LAN discovery (docs/api.md §15) ---------
    //
    // Numbers and strings cross separately, which is the JNI idiom this file already uses
    // for pd_device_status and the drawer facet: an IntArray the caller supplies is filled
    // by the glue, and the strings come back as the return value. Packing both into one
    // Array<String> would mean parsing integers out of text on the Kotlin side, which is
    // exactly the kind of quiet lossiness this SDK exists to avoid.

    /** Prints ONE diagnostic ticket through the full fenced engine (docs/api.md §15).
     *  USES PAPER. `values` must have room for 24 ints and receives, in order:
     *  [outcome, confidence, reason, grade, authority,
     *   identityTrusted, confidencePercent, impersonationSuspected, identityFresh,
     *   selection, nominalPaperMm, printableWidthDots, charsPerLine, dpi,
     *   completion, gradeCeiling, completionAuthority, completionProvenance,
     *   drawerPresent, drawerKickable, drawerStandard, drawerVoltage,
     *   degradationCount, ticketLineCount].
     *  Returns the strings: [resultMethod, key, printToken, endpoint, vendor, model,
     *  firmware, serial, profileId, completionMethod, provenanceSummary] followed by
     *  `degradationCount` degradation lines and then `ticketLineCount` ticket lines.
     *  Empty array when the call was refused; see driverLastError. Blocks. */
    @JvmStatic external fun selfTest(
        driverHandle: Long,
        printerHandle: Long,
        key: String?,
        refreshIdentity: Boolean,
        probeWithoutPrinting: Boolean,
        barcode: Boolean,
        barcodeData: String?,
        printVerificationId: Boolean,
        timeoutMs: Int,
        values: IntArray
    ): Array<String>

    /** Sweeps, identifies and classifies. NOTHING PRINTS AND NOTHING FIRES. Returns how
     *  many candidates were examined, or -1 on error (see driverLastError); read them
     *  back with [detectedAt]. Blocks. */
    @JvmStatic external fun autoDetect(
        driverHandle: Long,
        subnetCidr: String?,
        endpoints: Array<String>,
        port: Int,
        concurrency: Int,
        connectTimeoutMs: Int,
        responseTimeoutMs: Int,
        probeUnknown: Boolean
    ): Int

    /** One candidate of the last [autoDetect]. `values` must have room for 22 ints:
     *  [port, status, portOpen, fromCache, identityTrusted, confidencePercent,
     *   impersonationSuspected, identityFresh, selection, nominalPaperMm,
     *   printableWidthDots, charsPerLine, dpi, completion, gradeCeiling, authority,
     *   completionProvenance, drawerPresent, drawerKickable, drawerStandard,
     *   drawerVoltage, degradationCount].
     *  Returns [endpoint, host, dleEotHex, vendor, model, firmware, serial, profileId,
     *  completionMethod, provenanceSummary] plus `degradationCount` degradation lines.
     *  Empty array when the index is out of range. */
    @JvmStatic external fun detectedAt(driverHandle: Long, index: Int, values: IntArray): Array<String>

    /** The raw non-printing sweep beneath [autoDetect]: DLE EOT 1 and nothing else.
     *  Returns how many open ports were found, or -1 on error. Blocks. */
    @JvmStatic external fun discover(
        driverHandle: Long,
        subnetCidr: String?,
        port: Int,
        concurrency: Int,
        connectTimeoutMs: Int,
        responseTimeoutMs: Int,
        probeBackchannel: Boolean
    ): Int

    /** One device of the last [discover]. `values` must have room for 2 ints:
     *  [port, portOpen]. Returns [ip, dleEotHex], or an empty array when out of range. */
    @JvmStatic external fun discoveredAt(driverHandle: Long, index: Int, values: IntArray): Array<String>

    /** The local /24 as a CIDR string, or "" when it cannot be determined. */
    @JvmStatic external fun localSubnet(driverHandle: Long): String

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

    // --- M13b: the print-queue addon (docs/sdk-spec.md section 12) ------------------
    //
    // pd_queue* crosses as a Long like every other handle, with 0L as the failure
    // sentinel. It is the one handle the caller owns and frees: queueDestroy must run
    // before driverDestroy, because the addon is layered on the driver rather than being
    // part of its object graph.

    /** Returns 0L on failure -- check driverLastError. */
    @JvmStatic external fun queueCreate(
        driverHandle: Long,
        holdWhileOffline: Boolean,
        defaultTtlMs: Int,
        maxDepth: Int,
        drainOrder: Int
    ): Long

    /** Stops the queue thread. Held jobs stay held and stay non-terminal: the queue does
     *  not invent an outcome for a job whose fate it does not know. */
    @JvmStatic external fun queueDestroy(queueHandle: Long)

    /**
     * pd_queue_enqueue with a raw payload. Returns an ordinary job handle -- already sent
     * when the printer is usable and its lane is free, otherwise held, or already terminal
     * with QUEUE_OVERFLOW when the lane is full.
     *
     * Raw only in this milestone. The raster and document tiers reach the queue through
     * the same pd_queue_enqueue call and need the same two-pass marshaling printRaster and
     * printDocument already carry; until those entry points exist, a caller that wants a
     * queued document renders it and enqueues the bytes, which is a declared limitation
     * rather than a silent one.
     */
    @JvmStatic external fun queueEnqueueRaw(
        queueHandle: Long,
        printerHandle: Long,
        bytes: ByteArray,
        key: String?,
        ttlMs: Int,
        priority: Int,
        cut: Int,
        openDrawer: Boolean,
        preflight: Int,
        timeoutMs: Int
    ): Long

    @JvmStatic external fun queuePause(queueHandle: Long, printerId: String)

    @JvmStatic external fun queueResume(queueHandle: Long, printerId: String)

    @JvmStatic external fun queueIsPaused(queueHandle: Long, printerId: String): Boolean

    /** True once a job on this printer ended UNKNOWN. Nothing more drains onto that lane
     *  until queueUnblock -- an operator decision, never a timer. */
    @JvmStatic external fun queueIsBlocked(queueHandle: Long, printerId: String): Boolean

    @JvmStatic external fun queueUnblock(queueHandle: Long, printerId: String)

    /** Held jobs. A null or empty [printerId] counts every lane. */
    @JvmStatic external fun queuePending(queueHandle: Long, printerId: String?): Long

    @JvmStatic external fun queueExpiredCount(queueHandle: Long): Long

    @JvmStatic external fun queueOverflowCount(queueHandle: Long): Long

    @JvmStatic external fun queueDrainedCount(queueHandle: Long): Long

    /** One expiry-and-drain pass on the calling thread. The queue's own thread already
     *  does this on every device event and whenever a TTL comes due. */
    @JvmStatic external fun queueTick(queueHandle: Long)

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
