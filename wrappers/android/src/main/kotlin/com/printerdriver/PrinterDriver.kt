package com.printerdriver

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Context
import com.printerdriver.internal.NativeBridge
import com.printerdriver.internal.NativeLogCallback
import java.util.concurrent.ConcurrentHashMap
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

    // Keyed by printer handle so Printer.bluetoothTransportKind can answer §25's "which
    // of the five paths" question, and so close() can stop every reader thread before
    // pd_destroy frees the handles they feed.
    private val bluetoothTransports = ConcurrentHashMap<Long, BluetoothSppTransport>()

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

    /**
     * A printer reached over Bluetooth Classic SPP (docs/compatibility-brief.md §25),
     * using an `android.bluetooth.BluetoothSocket` this wrapper owns and drives.
     *
     * The device must already be paired -- pairing is an operator action through the
     * system's Bluetooth settings, and this SDK deliberately cannot perform it. The
     * calling app is responsible for holding `BLUETOOTH_CONNECT` and `BLUETOOTH_SCAN`
     * at runtime on API 31+; this library declares them but cannot request them, since
     * a runtime permission prompt needs an Activity.
     *
     * Throws [PrinterDriverException] on failure -- an unknown [BluetoothPrinterConfig.profileId]
     * or a vtable the core refused, both configuration mistakes. Note what is NOT a
     * failure here: the printer being out of range or switched off. Registration does not
     * connect; the core connects when it has something to send, and reports an
     * unreachable printer through the job's own [JobResult], which is where an operator
     * can act on it.
     *
     * NEVER RUN AGAINST HARDWARE. See the header comment of Bluetooth.kt for exactly what
     * has and has not been checked.
     *
     * @param adapter the system [BluetoothAdapter]; see the [Context] overload for the
     *   usual way to obtain one.
     */
    fun addPrinterBluetooth(adapter: BluetoothAdapter, config: BluetoothPrinterConfig): Printer {
        checkOpen()
        val transport = BluetoothSppTransport(adapter, config)
        val printerHandle = NativeBridge.addPrinterCustom(
            handle, transport, transport.description, config.profileId, config.widthDots
        )
        // Published even on failure: a reader thread the core may already have started
        // (see BluetoothSppTransport.attachTo) is waiting to be told, and 0L is how it
        // learns there is nothing to feed.
        transport.attachTo(handle, printerHandle)
        if (printerHandle == 0L) {
            throw PrinterDriverException("pd_add_printer_custom failed: $lastError")
        }
        bluetoothTransports[printerHandle] = transport
        return Printer(this, printerHandle)
    }

    /** [addPrinterBluetooth] with the adapter taken from the system service, which is
     *  the supported way to get one on the [minSdk][android.os.Build.VERSION_CODES.O]
     *  this library targets. Throws [PrinterDriverException] on a device with no
     *  Bluetooth hardware. */
    fun addPrinterBluetooth(context: Context, config: BluetoothPrinterConfig): Printer {
        val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        val adapter = manager?.adapter
            ?: throw PrinterDriverException("this device has no Bluetooth adapter")
        return addPrinterBluetooth(adapter, config)
    }

    internal fun bluetoothTransportFor(printerHandle: Long): BluetoothSppTransport? =
        bluetoothTransports[printerHandle]

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
     *
     * Custom transports are torn down BEFORE pd_destroy, not after: pd_destroy frees
     * every pd_printer handle, and a Bluetooth reader thread still feeding one would be
     * writing through a dangling pointer. See [BluetoothSppTransport.shutdown] for what
     * that costs an in-flight job.
     */
    override fun close() {
        if (closedFlag.compareAndSet(false, true)) {
            closureScope.cancel()
            for (transport in bluetoothTransports.values) {
                transport.shutdown()
            }
            bluetoothTransports.clear()
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
