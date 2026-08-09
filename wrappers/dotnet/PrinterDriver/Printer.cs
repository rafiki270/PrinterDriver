using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading.Channels;

namespace PrinterDriver;

/// <summary>
/// One printer, owned by the <see cref="PrinterDriver"/> that attached it.
/// </summary>
/// <remarks>
/// Nothing here is disposable: the driver owns every handle and frees them all in
/// <see cref="PrinterDriver.Dispose"/>.
/// </remarks>
public sealed class Printer
{
    private readonly PrinterDriver _driver;
    private readonly nint _handle;
    private readonly object _gate = new();
    private DeviceSubscription? _subscription;

    internal Printer(PrinterDriver driver, nint handle)
    {
        _driver = driver;
        _handle = handle;
        Id = NativeMethods.ReadUtf8(NativeMethods.pd_printer_id(handle));
    }

    /// <summary>The printer's stable identifier.</summary>
    public string Id { get; }

    /// <summary>Print width in dots.</summary>
    public uint WidthDots => NativeMethods.pd_printer_width_dots(_handle);

    /// <summary>
    /// Which ordered fence this printer's profile answers. <see cref="CompletionMechanism.None"/>
    /// means no job on it can ever do better than <see cref="JobResult.Unknown"/>.
    /// </summary>
    public CompletionMechanism Completion =>
        (CompletionMechanism)NativeMethods.pd_printer_completion(_handle);

    /// <summary>
    /// What the fence <see cref="Completion"/> names is actually worth
    /// (docs/compatibility-brief.md §28).
    /// </summary>
    /// <remarks>
    /// Not a confidence level, and it changes nothing about what a job reports. It answers
    /// a different question, before anything has been printed: should this printer's fence
    /// be trusted on the strength of its profile alone? Epson is the only family whose
    /// shipped defaults say <see cref="Provenance.Documented"/>, and
    /// <see cref="Provenance.Probed"/> means this driver asked the installed hardware over
    /// the installed interface path — which is what makes probing a site's machines worth
    /// the trip.
    /// </remarks>
    public Provenance CompletionProvenance =>
        (Provenance)NativeMethods.pd_printer_completion_provenance(_handle);

    /// <summary>
    /// The language this printer's profile is driven in. Anything but
    /// <see cref="CommandLanguage.EscPos"/> is refused with
    /// <see cref="FailureReason.Unsupported"/> before a byte is written.
    /// </summary>
    public CommandLanguage Language => (CommandLanguage)NativeMethods.pd_printer_language(_handle);

    /// <summary>
    /// Last known device state. Never a live query, so it cannot block behind a print.
    /// </summary>
    public DeviceStatus Status =>
        DeviceStatus.FromNative(NativeMethods.pd_printer_status(_driver.Handle, _handle));

    /// <summary>
    /// Queues a status round trip behind any active job and waits for the answer.
    /// </summary>
    /// <param name="timeoutMs">How long to wait; 0 means the ABI's default of 2000 ms.</param>
    /// <returns>The refreshed state.</returns>
    public DeviceStatus RefreshStatus(uint timeoutMs = 0) => DeviceStatus.FromNative(
        NativeMethods.pd_printer_refresh_status(_driver.Handle, _handle, timeoutMs));

    /// <summary>Kicks the cash drawer.</summary>
    public void OpenCashDrawer() => NativeMethods.pd_open_cash_drawer(_driver.Handle, _handle);

    /// <summary>Blocks until this printer's queue is empty and its active job is terminal.</summary>
    public void Drain() => NativeMethods.pd_printer_drain(_driver.Handle, _handle);

    /// <summary>
    /// Submits a job.
    /// </summary>
    /// <remarks>
    /// Re-submitting an idempotency key that already has a job does not print: it returns
    /// that job, whatever state it is in (docs/api.md §3). Because the driver interns
    /// handles, that shows up as the identical <see cref="PrintJob"/> instance.
    /// </remarks>
    /// <param name="payload">What to print.</param>
    /// <param name="options">Key, cut, preflight and timeout; null takes the defaults.</param>
    /// <returns>The job.</returns>
    /// <exception cref="PrinterDriverException">The payload was malformed.</exception>
    public PrintJob Print(Payload payload, JobOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(payload);
        options ??= new JobOptions();
        using var marshalled = new PayloadMarshaller(payload);
        var key = Marshal.StringToCoTaskMemUTF8(options.Key ?? string.Empty);
        try
        {
            var native = NativeOptions(options, key);
            var payloadStruct = marshalled.Native;
            var job = NativeMethods.pd_print(_driver.Handle, _handle, in payloadStruct, in native);
            if (job == 0)
            {
                throw new PrinterDriverException(_driver.LastError());
            }
            return _driver.InternJob(job);
        }
        finally
        {
            Marshal.FreeCoTaskMem(key);
        }
    }

    /// <summary>
    /// A deliberate duplicate of an already-submitted key: reuses the original payload and
    /// prepends the reprint banner and attempt counter.
    /// </summary>
    /// <param name="key">The original idempotency key.</param>
    /// <param name="options">Options for the reprint; null takes the defaults.</param>
    /// <returns>The new attempt.</returns>
    /// <exception cref="PrinterDriverException">
    /// The key is unknown, or its job was reconstructed from the journal — those records
    /// carry what happened to a job, never what it contained.
    /// </exception>
    public PrintJob ForceReprint(string key, JobOptions? options = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(key);
        options ??= new JobOptions();
        var keyPointer = Marshal.StringToCoTaskMemUTF8(options.Key ?? string.Empty);
        try
        {
            var native = NativeOptions(options, keyPointer);
            var job = NativeMethods.pd_force_reprint(_driver.Handle, _handle, key, in native);
            if (job == 0)
            {
                throw new PrinterDriverException(_driver.LastError());
            }
            return _driver.InternJob(job);
        }
        finally
        {
            Marshal.FreeCoTaskMem(keyPointer);
        }
    }

    /// <summary>
    /// Submits a job and awaits its terminal answer.
    /// </summary>
    /// <param name="payload">What to print.</param>
    /// <param name="options">Key, cut, preflight and timeout; null takes the defaults.</param>
    /// <param name="cancellationToken">Stops waiting; the job itself keeps going.</param>
    /// <returns>The tri-state result.</returns>
    public Task<JobResult> SendAsync(Payload payload, JobOptions? options = null,
                                     CancellationToken cancellationToken = default) =>
        Print(payload, options).GetResultAsync(cancellationToken);

    /// <summary>
    /// The device event stream, which is what replaces polling for availability.
    /// </summary>
    /// <remarks>
    /// Every subscriber reads the same underlying native subscription: pd.h has no
    /// unsubscribe, so this subscribes once, on first use, and fans out from there. A
    /// consumer that stops enumerating stops reading; it does not stop the core.
    /// </remarks>
    /// <param name="cancellationToken">Stops the enumeration.</param>
    /// <returns>Device events as they are decoded.</returns>
    public IAsyncEnumerable<DeviceEvent> EventsAsync(
        CancellationToken cancellationToken = default)
    {
        // Deliberately NOT an `async IAsyncEnumerable` method. The body of one of those
        // does not run until the first MoveNextAsync, which would leave a window between
        // "the caller asked for events" and "this printer is actually listening" -- and
        // every event decoded inside that window would be lost. Registering eagerly and
        // returning the reader's own enumerable closes it.
        var reader = Subscription().Add();
        return reader.ReadAllAsync(cancellationToken);
    }

    /// <summary>
    /// Device events as a classic .NET event, for call sites that would rather not
    /// enumerate. Handlers run on whichever core thread decoded the status — the
    /// transport's reader thread, or a worker thread when a status query answers inside a
    /// job — so they must not block and must not call back into this SDK.
    /// </summary>
    public event EventHandler<DeviceEvent>? DeviceEventReceived
    {
        add => Subscription().Handlers += value;
        remove => Subscription().Handlers -= value;
    }

    internal nint Handle => _handle;

    private DeviceSubscription Subscription()
    {
        lock (_gate)
        {
            if (_subscription is not null)
            {
                return _subscription;
            }
            var subscription = new DeviceSubscription(this);
            // Root the state object and the delegate before handing either to the ABI:
            // after pd_subscribe_device returns, a worker thread may call the delegate at
            // any moment, and nothing else in the managed heap refers to it.
            var context = _driver.Roots.Root(subscription);
            var callback = new NativeMethods.DeviceEventCallback(DeviceSubscription.Trampoline);
            _driver.Roots.Root(callback);
            NativeMethods.pd_subscribe_device(_driver.Handle, _handle, callback,
                                              GCHandle.ToIntPtr(context));
            _subscription = subscription;
            return subscription;
        }
    }

    private static PdJobOptions NativeOptions(JobOptions options, nint key) => new()
    {
        Key = key,
        Cut = (int)options.Cut,
        OpenDrawer = options.OpenDrawer ? 1 : 0,
        Preflight = (int)options.Preflight,
        TimeoutMs = options.TimeoutMs,
    };

    /// <summary>The one native device subscription, fanned out to any number of readers.</summary>
    private sealed class DeviceSubscription(Printer printer)
    {
        private readonly List<Channel<DeviceEvent>> _channels = [];
        private readonly object _gate = new();

        internal EventHandler<DeviceEvent>? Handlers;

        internal ChannelReader<DeviceEvent> Add()
        {
            var channel = Channel.CreateUnbounded<DeviceEvent>(new UnboundedChannelOptions
            {
                SingleWriter = false,
                SingleReader = true,
            });
            lock (_gate)
            {
                _channels.Add(channel);
            }
            return channel.Reader;
        }

        internal static void Trampoline(nint _, int deviceEvent, nint context)
        {
            var subscription = CallbackRoots.FromContext<DeviceSubscription>(context);
            subscription?.Publish((DeviceEvent)deviceEvent);
        }

        private void Publish(DeviceEvent deviceEvent)
        {
            Channel<DeviceEvent>[] channels;
            lock (_gate)
            {
                channels = [.. _channels];
            }
            foreach (var channel in channels)
            {
                channel.Writer.TryWrite(deviceEvent);
            }
            // pd.h forbids blocking or re-entering the ABI from a callback, so a handler
            // that throws must not take the worker thread down with it -- and must not be
            // allowed to stop the other handlers either.
            foreach (var handler in Handlers?.GetInvocationList() ?? [])
            {
                try
                {
                    ((EventHandler<DeviceEvent>)handler)(printer, deviceEvent);
                }
                catch (Exception)
                {
                    // Deliberately swallowed: see above.
                }
            }
        }
    }
}
