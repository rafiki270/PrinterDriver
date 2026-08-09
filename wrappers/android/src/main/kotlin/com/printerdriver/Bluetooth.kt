package com.printerdriver

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import com.printerdriver.internal.NativeBridge
import com.printerdriver.internal.NativeTransportCallback
import com.printerdriver.internal.PdLog
import java.io.IOException
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

// Bluetooth Classic SPP (RFCOMM) for Android, over pd.h's custom-transport ABI
// (docs/compatibility-brief.md §25).
//
// -- Why it lives here and not in the core ------------------------------------------
//
// §25's split: THE PLATFORM OWNS THE SOCKET, THE CORE OWNS THE PROTOCOL. Everything that
// makes this SDK worth using -- the ordered fence, the GS(H) correlation token,
// preflight, the journal, the confidence grading, the refusal to overclaim -- stays on
// the C++ side and behaves identically over Bluetooth, TCP or a test double. This file
// answers exactly one question, "can bytes move?", and is structurally incapable of
// answering "did the receipt print?" -- which is why a Bluetooth wrapper cannot weaken a
// completion guarantee by accident.
//
// -- VERIFICATION STATUS -------------------------------------------------------------
//
// NEVER RUN. Not on a device, not on an emulator, not in a unit test. What has actually
// been done to this file is: it was written by hand and read back. It has NOT been
// compiled -- there is no JVM, no kotlinc, no Gradle and no Android SDK on the authoring
// machine (README.md "Verification status"), so not even a type error would have been
// caught here. The only mechanical check that ran anywhere near it is the host-side
// clang++ -fsyntax-only pass over src/main/cpp/printerdriver_jni.cpp against
// src/test/cpp/jni_stub.h, which proves that the three JNI entry points this file calls
// exist with matching C++ signatures and that the vtable it registers matches the real
// pd.h. That says nothing whatsoever about this Kotlin file.
//
// Specifically unproven, in rough order of how likely each is to bite:
//   1. That the JNI signature strings "()Z", "([B)I" and "()V" that
//      printerdriver_jni.cpp looks up actually match what kotlinc emits for
//      NativeTransportCallback's three methods on a real build.
//   2. That an RFCOMM connection to any real printer succeeds at all -- no paired
//      device has ever been in the room. Every address, channel and UUID assumption
//      below comes from documentation, not from an observed connection.
//   3. That the handle-publication latch's timing assumption holds under a real core
//      (see attachTo) rather than merely being sound on paper.
//   4. That BLUETOOTH_CONNECT / BLUETOOTH_SCAN are the complete and correct API 31+
//      permission set for the calls made here, and that the SecurityException paths
//      trigger where expected on a device that has not granted them.
// Hardware-pending. Until a paired printer answers, this is a design, not a driver.

/**
 * Which of the five Bluetooth paths a transport actually uses
 * (docs/compatibility-brief.md §25).
 *
 * §25's rule is "never `bluetooth: true`": five different things hide behind that
 * boolean and they need five different stacks. An Epson TM-P20II documents Classic
 * *and* BLE; a Star SM-S230i is driven through Star's own SDK; Citizen CMP sells an MFi
 * variant; Zebra exposes separate Classic and BLE status connections. Which path is in
 * use changes the receive-buffer size, the pacing, and whether a status backchannel
 * exists at all, so it is never a detail.
 *
 * This is a wrapper-owned enum, deliberately NOT in Enums.kt: it mirrors no
 * `pd_*` enum, because the C ABI does not expose one. The core's own
 * `pd::BluetoothTransport` (core/include/printerdriver/capability_profile.hpp) carries
 * these as five independent booleans on a *printer's capability profile* -- what a given
 * device supports -- and pd.h has no accessor for a capability profile at all, so this
 * wrapper cannot report that and does not pretend to. What it can report is which path a
 * transport it created is driving, which is [BluetoothSppTransport]'s [CLASSIC_SPP] and
 * nothing else today.
 */
enum class BluetoothTransportKind {
    /** Bluetooth Classic, Serial Port Profile: a plain byte stream. The only kind this
     *  wrapper implements. */
    CLASSIC_SPP,

    /** Classic, but under the vendor's own framing rather than a raw stream. */
    CLASSIC_VENDOR,

    /** Bluetooth Low Energy, GATT. */
    BLE,

    /** Apple MFi / ExternalAccessory -- iOS only, listed so the set stays the §25 set. */
    MFI,

    /** The manufacturer's documented path is its SDK, not a socket anyone else can open. */
    VENDOR_SDK
}

/**
 * Configuration for [PrinterDriver.addPrinterBluetooth].
 *
 * Pairing is deliberately not part of this: it is an operator action through the
 * system's own Bluetooth settings, and a printing SDK that can pair can also pair with
 * the wrong printer. This connects to a device that is already paired.
 */
data class BluetoothPrinterConfig(
    /** The paired device's address, "00:11:22:33:44:55". Upper-case hex, as
     *  [BluetoothAdapter.checkBluetoothAddress] requires. */
    val address: String,

    /** 0 -> 576. 384, 504 and 576 are the deployed widths (docs/api.md §2). Portable
     *  Bluetooth printers are commonly 384 (58mm), so this is worth setting. */
    val widthDots: Int = 0,

    /** `null`/`""` -> "generic". See [PrinterDriver.profileIds] for the accepted set. */
    val profileId: String? = null,

    /**
     * `true` uses `createRfcommSocketToServiceRecord` (encrypted, authenticated);
     * `false` uses the insecure variant.
     *
     * Secure is the default because it is the one that does not silently downgrade the
     * link. Some older printers only accept the insecure socket -- that is a real,
     * documented incompatibility rather than a bug, and it is the caller's call to make,
     * not this wrapper's.
     */
    val secure: Boolean = true,

    /** Bytes per [java.io.InputStream.read] on the reader thread. The backchannel carries
     *  status frames and GS(H) echoes -- tens of bytes, not kilobytes -- so this is
     *  sized for latency, not throughput. */
    val readBufferBytes: Int = 512
)

/**
 * A [NativeTransportCallback] backed by an `android.bluetooth.BluetoothSocket` over
 * RFCOMM.
 *
 * `internal`, and reached only through [PrinterDriver.addPrinterBluetooth]: constructing
 * one directly would skip the driver registration that shuts its reader thread down
 * before `pd_destroy` frees the printer handle it feeds. The public surface is
 * [BluetoothPrinterConfig] going in and [Printer.bluetoothTransportKind] coming out.
 *
 * On the visibility, which matters here in a way it usually does not: Kotlin mangles the
 * compiled names of `internal` members, and `connect`/`write`/`close` are looked up by
 * literal name through `GetMethodID` (see NativeCallbacks.kt's header for the full
 * argument). The three below are safe because they are `override`s -- an override cannot
 * be mangled without breaking the override itself, which is why an internal Kotlin class
 * can implement `java.lang.Runnable` and still produce a callable `run()`. None of the
 * three is marked `internal` individually, and none should be. Confirmed by reasoning,
 * not by reading `javap` output -- README.md "What a real CI run must confirm", item 5.
 *
 * See this file's header for what has and has not been verified. In short: nothing here
 * has ever executed.
 */
internal class BluetoothSppTransport(
    private val adapter: BluetoothAdapter,
    private val config: BluetoothPrinterConfig
) : NativeTransportCallback {

    /** Always [BluetoothTransportKind.CLASSIC_SPP] -- see that enum for why this is a
     *  facet rather than a boolean. */
    val kind: BluetoothTransportKind = BluetoothTransportKind.CLASSIC_SPP

    /** What the printer id and the native diagnostics derive from, per pd.h's own
     *  example spelling for this transport. */
    internal val description: String = "bt-spp:${config.address}"

    // The printer handle the reader thread feeds bytes into. Not known when this object
    // is constructed, and -- see attachTo -- not necessarily known by the time the core
    // first calls connect() either.
    @Volatile private var driverHandle: Long = 0L
    @Volatile private var printerHandle: Long = 0L
    private val handlePublished = CountDownLatch(1)

    // One per connect(). The core reconnects after a link drop by calling connect()
    // again on this same object (pd.h: "the registration outlives individual
    // connections"), so per-connection state cannot live in a field that close() poisons
    // permanently -- it lives in a session that connect() replaces.
    @Volatile private var session: Session? = null

    private class Session(val socket: BluetoothSocket) {
        val closing = AtomicBoolean(false)
        @Volatile var reader: Thread? = null
    }

    /**
     * Publishes the printer handle this transport feeds. Called once by
     * [PrinterDriver.addPrinterBluetooth] immediately after `pd_add_printer_custom`
     * returns, with 0L when it failed.
     *
     * This exists because of a genuine ordering hazard rather than for tidiness:
     * `pd_add_printer_custom` starts the printer's worker thread and queues a capability
     * probe on it before returning, so [connect] can run -- and the reader thread can
     * start -- before there is any handle to feed. The reader waits on
     * [handlePublished] before its first read rather than dropping those bytes on the
     * floor; they stay in the socket's own receive buffer meanwhile, exactly where they
     * would be if the thread had simply not been scheduled yet.
     */
    internal fun attachTo(driver: Long, printer: Long) {
        driverHandle = driver
        printerHandle = printer
        handlePublished.countDown()
    }

    // --- NativeTransportCallback: called BY THE CORE on the printer's worker thread ----

    override fun connect(): Boolean {
        if (!adapter.isEnabled) {
            PdLog.w("$description: Bluetooth adapter is off")
            return false
        }
        if (!BluetoothAdapter.checkBluetoothAddress(config.address)) {
            PdLog.w("$description: not a valid Bluetooth address")
            return false
        }
        // Held outside the try so a failure part-way through can close the socket it
        // already created. A BluetoothSocket that is dropped unclosed holds an RFCOMM
        // channel open until the finalizer runs, and a printer that only accepts one
        // connection then refuses every retry -- a leak that presents as "the printer
        // stopped working" rather than as a leak.
        var opened: BluetoothSocket? = null
        return try {
            val device: BluetoothDevice = adapter.getRemoteDevice(config.address)
            // Discovery is a heavyweight radio operation that starves an RFCOMM connect
            // and then the link itself; Android's own documentation makes cancelling it
            // a precondition, not an optimisation. Cancelling one this wrapper did not
            // start is deliberate -- the radio is shared, and a discovery running
            // elsewhere in the app breaks this connect just as effectively as one
            // started here.
            adapter.cancelDiscovery()

            val socket = if (config.secure) {
                device.createRfcommSocketToServiceRecord(SPP_UUID)
            } else {
                device.createInsecureRfcommSocketToServiceRecord(SPP_UUID)
            }
            opened = socket
            socket.connect()

            val newSession = Session(socket)
            session = newSession
            val reader = Thread({ readerLoop(newSession) }, "pd-bt-rx-${config.address}")
            reader.isDaemon = true
            newSession.reader = reader
            reader.start()
            true
        } catch (io: IOException) {
            // Out of range, not actually paired, printer busy with another host, SPP
            // record absent. All routine; the core retries on the next job and reports
            // TRANSPORT_UNREACHABLE if it keeps failing.
            PdLog.w("$description: RFCOMM connect failed: ${io.message}")
            closeQuietly(opened)
            false
        } catch (denied: SecurityException) {
            // API 31+ without BLUETOOTH_CONNECT granted. A configuration mistake in the
            // host app, not a printer problem, so it is logged distinctly.
            PdLog.w("$description: BLUETOOTH_CONNECT not granted: ${denied.message}")
            closeQuietly(opened)
            false
        }
    }

    override fun write(data: ByteArray): Int {
        val socket = session?.socket ?: return -1
        var written = 0
        return try {
            val output = socket.outputStream
            // Chunked, and the count accumulated as it goes, so a link that dies
            // mid-receipt reports how far it got. BluetoothSocket's OutputStream is
            // all-or-throw per call, so a single write(data) could only ever report
            // data.size or -1 -- and the difference between "zero bytes out" (a known
            // failure, safe to resubmit) and "some bytes out" (Unknown, ask the
            // operator) is precisely what pd.h §4 turns into an operator decision. The
            // chunk size therefore buys failure resolution, not throughput, and it does
            // not control RFCOMM framing: the count reported is bytes accepted by the
            // OS, which is the same claim a TCP send() makes and is not a claim about
            // what the printer received.
            while (written < data.size) {
                val chunk = minOf(WRITE_CHUNK_BYTES, data.size - written)
                output.write(data, written, chunk)
                written += chunk
            }
            output.flush()
            written
        } catch (io: IOException) {
            PdLog.w("$description: write failed after $written of ${data.size} bytes: ${io.message}")
            // Zero out is a hard failure; anything else is the honest partial count,
            // which the core turns into Unknown rather than Failed.
            if (written == 0) -1 else written
        }
    }

    override fun close() {
        // Runs on the printer's worker thread. It closes the socket, which is what
        // unblocks the reader's blocking read(), but deliberately does NOT join that
        // thread: pd.h allows the core to call connect() again straight afterwards, and
        // making a reconnect wait on a thread that is itself finishing a
        // pd_transport_feed_bytes call would put the worker thread behind the very ABI
        // it owns. The reader observes `closing` and exits on its own; joining it is
        // shutdown()'s job, on the app's thread, where waiting is free.
        closeSessionQuietly()
    }

    // --- Reader thread: the ONLY place pd_transport_feed_bytes is called from ---------

    private fun readerLoop(active: Session) {
        // pd.h forbids feeding bytes from inside connect/write/close because those run on
        // the thread that would have to service the delivery. This loop is a thread this
        // wrapper owns and the core knows nothing about, which is exactly what the ABI
        // asks for.
        if (!handlePublished.await(HANDLE_WAIT_MS, TimeUnit.MILLISECONDS)) {
            PdLog.w("$description: no printer handle after ${HANDLE_WAIT_MS}ms; reader giving up")
            return
        }
        val printer = printerHandle
        val driver = driverHandle
        if (printer == 0L || driver == 0L) {
            return // pd_add_printer_custom failed; there is nothing to feed.
        }

        val buffer = ByteArray(config.readBufferBytes.coerceAtLeast(1))
        val input = try {
            active.socket.inputStream
        } catch (io: IOException) {
            PdLog.w("$description: no input stream: ${io.message}")
            return
        }

        var dropMessage: String? = null
        while (!active.closing.get()) {
            val count = try {
                input.read(buffer)
            } catch (io: IOException) {
                // Distinguishing "we closed it" from "it went away" is the whole point:
                // only the latter is a link drop the core should hear about.
                if (!active.closing.get()) dropMessage = io.message ?: "RFCOMM read failed"
                break
            }
            if (count < 0) {
                if (!active.closing.get()) dropMessage = "RFCOMM stream reached end of file"
                break
            }
            if (count > 0) {
                NativeBridge.transportFeedBytes(driver, printer, buffer, count)
            }
        }

        dropMessage?.let { message ->
            // Surfaces as DeviceEvent.CONNECTION_LOST and fails any job waiting on a
            // fence, instead of leaving it to time out (pd.h pd_transport_link_dropped).
            NativeBridge.transportLinkDropped(driver, printer, "$description: $message")
        }
    }

    // --- Lifecycle -------------------------------------------------------------------

    /**
     * Stops the reader thread and closes the socket, from the app's thread.
     *
     * Called by [PrinterDriver.close] BEFORE `pd_destroy`, because `pd_destroy` frees
     * every pd_printer handle and a reader thread outliving it would feed bytes through
     * a dangling pointer. The native side refuses post-destroy feeds as well (see
     * printerdriver_jni.cpp's lifecycle mutex); this is the orderly half of that pair,
     * and the reason a shutdown does not depend on a race being lost.
     *
     * The consequence is worth stating plainly: an in-flight job loses its backchannel at
     * this point and will settle as Failed or Unknown rather than waiting for a fence
     * that can no longer arrive. That is the correct trade against a use-after-free, and
     * an app that wants in-flight jobs to complete should await them before closing.
     */
    internal fun shutdown() {
        val active = session
        closeSessionQuietly()
        // Bounded rather than indefinite: a reader stuck inside a native call must not
        // be able to hang an app's shutdown path. The native lifecycle guard is what
        // makes a straggler safe rather than merely unlikely.
        active?.reader?.join(READER_JOIN_MS)
        printerHandle = 0L
        driverHandle = 0L
        handlePublished.countDown() // Releases a reader still waiting for a handle.
    }

    private fun closeSessionQuietly() {
        val active = session ?: return
        // Set before the close so the reader can tell "we closed it" from "it went
        // away" -- only the latter is a link drop the core should hear about.
        active.closing.set(true)
        closeQuietly(active.socket)
    }

    private fun closeQuietly(socket: BluetoothSocket?) {
        try {
            socket?.close()
        } catch (io: IOException) {
            // Closing a socket that is already dead is the expected case, not an error
            // worth surfacing: close() runs on every teardown path, including the one
            // that runs precisely because the link already failed.
            PdLog.w("$description: socket close: ${io.message}")
        }
    }

    private companion object {
        /**
         * The Serial Port Profile UUID. Every Classic SPP receipt printer in
         * docs/compatibility-brief.md §26 advertises this well-known record; it is not a
         * per-vendor value.
         *
         * `8000` in the third group, not `0000`: this is the 16-bit SPP identifier
         * `0x1101` expanded through the Bluetooth Base UUID
         * `00000000-0000-1000-8000-00805F9B34FB`. The transcription that drops the
         * `8000` is a common one and produces a UUID no device advertises, so it fails
         * as an SDP lookup miss rather than as anything that names the real mistake.
         */
        val SPP_UUID: UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB")

        const val WRITE_CHUNK_BYTES = 512

        /** How long a reader waits to be told its printer handle. Generous by orders of
         *  magnitude -- the publication happens microseconds after
         *  pd_add_printer_custom returns -- so exceeding it means the registration
         *  failed in a way nobody reported, which is worth a log line. */
        const val HANDLE_WAIT_MS = 5_000L

        const val READER_JOIN_MS = 2_000L
    }
}
