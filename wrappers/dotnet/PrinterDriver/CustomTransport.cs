using System.Runtime.InteropServices;

namespace PrinterDriver;

/// <summary>
/// A link the caller owns: Bluetooth Classic SPP, BLE, MFi, a vendor SDK channel, a USB
/// bulk pipe, a test double.
/// </summary>
/// <remarks>
/// <para>
/// docs/compatibility-brief.md §25. Bluetooth cannot live in the portable core and should
/// not: on Windows it is the WinRT <c>Rfcomm</c> stack, on Android a
/// <c>BluetoothSocket</c>, on Apple CoreBluetooth or ExternalAccessory. Each is a platform
/// framework with its own permissions model, pairing UI and threading. So the split is
/// <b>the platform owns the socket, the core owns the protocol</b>: an implementation of
/// this interface answers "can bytes move?", and nothing else. The ordered fence, the
/// GS(H) correlation token, preflight, the journal, the confidence grading and the refusal
/// to overclaim all stay on the core's side and behave identically over Bluetooth, TCP or
/// a test double — which is why an implementation of this interface cannot weaken a
/// completion guarantee by accident. It never makes one.
/// </para>
/// <para>
/// <b>Thread contract.</b> <see cref="Connect"/>, <see cref="Write"/> and
/// <see cref="Close"/> are invoked on the core's worker thread for this printer, one at a
/// time and never concurrently with each other. None of them may call back into this SDK
/// on the same driver — including
/// <see cref="CustomTransportPrinter.FeedBytes(ReadOnlySpan{byte})"/>, which is refused
/// outright if attempted, because the worker thread they are running on is the thread that
/// would have to service the call. Received bytes are pushed in from the caller's own RX
/// thread instead, and may be pushed while a <see cref="Write"/> is still in flight; that
/// is the normal case, not an edge case, because a status answer arrives while the next
/// chunk is going out.
/// </para>
/// <para>
/// The registration outlives individual connections. After a link drop the core reconnects
/// by calling <see cref="Connect"/> again on the same instance, so an implementation keeps
/// feeding the same <see cref="CustomTransportPrinter"/> and never has to learn about it.
/// </para>
/// </remarks>
public interface IPrinterTransport
{
    /// <summary>Opens the link.</summary>
    /// <returns>True when it is open and ready to carry bytes.</returns>
    bool Connect();

    /// <summary>
    /// Hands bytes to the link.
    /// </summary>
    /// <remarks>
    /// A short write must be reported honestly rather than rounded up to the full length:
    /// zero bytes out is a known failure and one byte out is Unknown (docs/api.md §4), and
    /// that difference is what decides whether an operator should reprint.
    /// <paramref name="data"/> points into the core's own send buffer and is valid only
    /// until this call returns; copy anything that has to outlive it.
    /// </remarks>
    /// <param name="data">The bytes to transfer.</param>
    /// <returns>Bytes actually transferred, or a negative value for a hard failure.</returns>
    long Write(ReadOnlySpan<byte> data);

    /// <summary>
    /// Closes the link. Called once per successful <see cref="Connect"/>, and calling it
    /// again is harmless.
    /// </summary>
    void Close();
}

/// <summary>
/// A printer attached over an <see cref="IPrinterTransport"/>, paired with the two calls
/// that push what the link received back into the core.
/// </summary>
/// <remarks>
/// A separate type from <c>Printer</c> on purpose: <see cref="FeedBytes"/> and
/// <see cref="LinkDropped"/> mean nothing on a TCP printer, where the core owns the socket
/// and reads it itself. Keeping them off <c>Printer</c> is what makes that unrepresentable
/// rather than merely undocumented.
/// </remarks>
public sealed class CustomTransportPrinter
{
    private readonly PrinterDriver _driver;

    internal CustomTransportPrinter(PrinterDriver driver, Printer printer)
    {
        _driver = driver;
        Printer = printer;
    }

    /// <summary>The printer itself: print, subscribe, drain, read status.</summary>
    public Printer Printer { get; }

    /// <summary>
    /// Delivers bytes the link received.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Safe from any thread, including while an <see cref="IPrinterTransport.Write"/> is in
    /// flight — but never from inside <see cref="IPrinterTransport.Connect"/>,
    /// <see cref="IPrinterTransport.Write"/> or <see cref="IPrinterTransport.Close"/>. That
    /// case throws rather than deadlocking, because the alternative is a worker thread
    /// waiting on itself with nothing in the stack trace to say so.
    /// </para>
    /// <para>
    /// Stop the RX thread before disposing the driver. <c>pd_destroy</c> frees every
    /// printer handle, and a delivery that starts after that is reading freed memory; the
    /// disposal check below closes the window it can, not the one between the check and
    /// the native call.
    /// </para>
    /// </remarks>
    /// <param name="data">What arrived on the link.</param>
    /// <returns>
    /// True when the bytes reached the core's response parser. False when nothing was
    /// listening — the core has not connected yet, or has already closed — which is
    /// information rather than an error: bytes arriving with no reader are dropped, exactly
    /// as they would be on a socket nobody is reading.
    /// </returns>
    /// <exception cref="InvalidOperationException">
    /// Called from inside a transport callback.
    /// </exception>
    /// <exception cref="ObjectDisposedException">The driver has been disposed.</exception>
    public unsafe bool FeedBytes(ReadOnlySpan<byte> data)
    {
        TransportBinding.ThrowIfInsideCallback(nameof(FeedBytes));
        _ = _driver.Handle;
        if (data.IsEmpty)
        {
            return false;
        }
        fixed (byte* pointer = data)
        {
            return NativeMethods.pd_transport_feed_bytes(Printer.Handle, (nint)pointer,
                                                         (nuint)data.Length) != 0;
        }
    }

    /// <summary>
    /// Reports that the link dropped for a reason other than an explicit close — the peer
    /// went out of range, the OS tore the channel down, pairing was revoked.
    /// </summary>
    /// <remarks>
    /// Surfaces as <see cref="DeviceEvent.ConnectionLost"/> and fails any job waiting on a
    /// fence, instead of leaving it to sit out the profile's completion timeout.
    /// </remarks>
    /// <param name="message">What happened, for the diagnostics.</param>
    /// <returns>True when a live transport was notified, false when there was none.</returns>
    /// <exception cref="InvalidOperationException">
    /// Called from inside a transport callback.
    /// </exception>
    /// <exception cref="ObjectDisposedException">The driver has been disposed.</exception>
    public bool LinkDropped(string message = "link dropped")
    {
        TransportBinding.ThrowIfInsideCallback(nameof(LinkDropped));
        _ = _driver.Handle;
        return NativeMethods.pd_transport_link_dropped(Printer.Handle, message ?? "link dropped")
            != 0;
    }
}

/// <summary>
/// Holds one <see cref="IPrinterTransport"/> on the managed side of the ABI and answers the
/// three unmanaged upcalls into it.
/// </summary>
internal sealed class TransportBinding
{
    // Set for exactly as long as this thread is inside connect/write/close. pd.h forbids
    // feeding bytes from there, and the failure mode if it happens anyway is not an
    // exception but a worker thread blocking on work only it can do -- so the wrapper
    // detects the caller's mistake instead of inheriting the deadlock. Thread-local rather
    // than per-binding because the condition being tested is about the calling thread, not
    // about which printer it belongs to.
    [ThreadStatic] private static bool _insideCallback;

    private readonly IPrinterTransport _transport;

    internal TransportBinding(IPrinterTransport transport) => _transport = transport;

    internal static void ThrowIfInsideCallback(string operation)
    {
        if (_insideCallback)
        {
            throw new InvalidOperationException(
                $"{operation} was called from inside Connect, Write or Close. Those run on " +
                "the core's worker thread for this printer, which is the thread that would " +
                "have to service the delivery, so it would wait on itself. Hand the bytes " +
                "to your own RX thread and feed them from there.");
        }
    }

    internal static int ConnectTrampoline(nint context)
    {
        var binding = CallbackRoots.FromContext<TransportBinding>(context);
        if (binding is null)
        {
            return 0;
        }
        var previous = _insideCallback;
        _insideCallback = true;
        try
        {
            return binding._transport.Connect() ? 1 : 0;
        }
        catch (Exception)
        {
            // A managed exception must never unwind into the core's worker thread: crossing
            // an unmanaged frame tears the process down. pd.h already has a vocabulary for
            // "this did not work" -- 0 from connect, a negative from write -- so a throwing
            // transport is reported in that vocabulary, and the job fails with a transport
            // reason rather than taking the application with it.
            return 0;
        }
        finally
        {
            _insideCallback = previous;
        }
    }

    internal static unsafe long WriteTrampoline(nint context, nint data, nuint size)
    {
        var binding = CallbackRoots.FromContext<TransportBinding>(context);
        if (binding is null || data == 0)
        {
            return -1;
        }
        if (size > int.MaxValue)
        {
            // Unreachable through this core, which chunks to the profile's buffer size; a
            // span cannot describe it, and inventing a byte count would be the one lie this
            // ABI has no room for.
            return -1;
        }
        var previous = _insideCallback;
        _insideCallback = true;
        try
        {
            return binding._transport.Write(new ReadOnlySpan<byte>((void*)data, (int)size));
        }
        catch (Exception)
        {
            return -1;
        }
        finally
        {
            _insideCallback = previous;
        }
    }

    internal static void CloseTrampoline(nint context)
    {
        var binding = CallbackRoots.FromContext<TransportBinding>(context);
        if (binding is null)
        {
            return;
        }
        var previous = _insideCallback;
        _insideCallback = true;
        try
        {
            binding._transport.Close();
        }
        catch (Exception)
        {
            // Same reason as ConnectTrampoline, and there is no return value to carry it.
            // A close that throws has already lost the link either way.
        }
        finally
        {
            _insideCallback = previous;
        }
    }
}
