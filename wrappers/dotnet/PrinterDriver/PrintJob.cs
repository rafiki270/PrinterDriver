using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading.Channels;

namespace PrinterDriver;

/// <summary>
/// One submitted job. Owned by its <see cref="PrinterDriver"/>; never disposed
/// individually.
/// </summary>
public sealed class PrintJob
{
    private readonly PrinterDriver _driver;
    private readonly nint _handle;
    private readonly object _gate = new();
    private readonly List<Channel<JobEvent>> _channels = [];
    private bool _subscribed;
    private bool _driverDisposed;

    internal PrintJob(PrinterDriver driver, nint handle)
    {
        _driver = driver;
        _handle = handle;
        Id = NativeMethods.ReadUtf8(NativeMethods.pd_job_id(handle));
        Key = NativeMethods.ReadUtf8(NativeMethods.pd_job_key(handle));
    }

    /// <summary>The job's own identifier, stable for the life of the driver.</summary>
    public string Id { get; }

    /// <summary>The idempotency key it was submitted under.</summary>
    public string Key { get; }

    /// <summary>Which attempt this is; a force-reprint increments it.</summary>
    public uint Attempt => NativeMethods.pd_job_attempt(_handle);

    /// <summary>Where the job is right now.</summary>
    public JobState CurrentState => (JobState)NativeMethods.pd_job_current_state(_handle);

    /// <summary>What the current state rests on.</summary>
    public ConfidenceLevel Confidence => (ConfidenceLevel)NativeMethods.pd_job_confidence(_handle);

    /// <summary>True once the job has reached a terminal state.</summary>
    public bool IsTerminal => NativeMethods.pd_job_is_terminal(_handle) != 0;

    /// <summary>
    /// The verification identifier this job's ticket printed as <c>V:</c> — the wire token
    /// its print fence carried, promoted to paper (docs/api.md §14).
    /// </summary>
    /// <remarks>
    /// Null on a printer whose completion mechanism is not <c>GS ( H</c> (there is no wire
    /// token to promote) and until the job reaches a worker. Once non-null it never
    /// changes, and <see cref="PrinterDriver.JobByToken(string)"/> resolves it back to this
    /// job, including after a restart.
    /// </remarks>
    public string? PrintToken
    {
        get
        {
            var token = NativeMethods.ReadUtf8(NativeMethods.pd_job_print_token(_handle));
            return token.Length == 0 ? null : token;
        }
    }

    /// <summary>The identifier this job's cut fence carried. Same rules as
    /// <see cref="PrintToken"/>.</summary>
    public string? CutToken
    {
        get
        {
            var token = NativeMethods.ReadUtf8(NativeMethods.pd_job_cut_token(_handle));
            return token.Length == 0 ? null : token;
        }
    }

    /// <summary>
    /// The job's full event stream: every event recorded so far, replayed on the calling
    /// thread, then the rest as the printer's worker thread produces them. The last event
    /// a job ever emits is always a terminal one, which is what ends the enumeration.
    /// </summary>
    /// <param name="cancellationToken">Stops the enumeration; the job keeps going.</param>
    /// <returns>The events, in order.</returns>
    public IAsyncEnumerable<JobEvent> EventsAsync(
        CancellationToken cancellationToken = default)
    {
        // Subscribe eagerly, before the iterator is ever advanced: the body of an
        // `async IAsyncEnumerable` method does not run until the first MoveNextAsync, and
        // anything the worker thread emitted in between would be gone.
        var reader = Subscribe();
        return Enumerate(reader, cancellationToken);
    }

    private static async IAsyncEnumerable<JobEvent> Enumerate(
        ChannelReader<JobEvent> reader,
        [EnumeratorCancellation] CancellationToken cancellationToken)
    {
        await foreach (var jobEvent in reader.ReadAllAsync(cancellationToken)
                           .ConfigureAwait(false))
        {
            yield return jobEvent;
            if (jobEvent.IsTerminal)
            {
                yield break;
            }
        }
    }

    /// <summary>
    /// Waits for the terminal result.
    /// </summary>
    /// <remarks>
    /// Implemented as a short polling loop over <c>pd_job_await</c> rather than one
    /// indefinite native call, specifically so that a cancelled token is noticed within
    /// one poll interval: <c>pd_job_await</c> itself has no cancellation hook, and a
    /// thread parked inside it cannot be interrupted.
    /// </remarks>
    /// <param name="cancellationToken">Stops waiting; the job itself keeps going.</param>
    /// <returns>Done, Failed or Unknown — never a boolean.</returns>
    public async Task<JobResult> GetResultAsync(CancellationToken cancellationToken = default)
    {
        const uint pollMs = 100;
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var result = await Task.Run(() =>
            {
                var ready = NativeMethods.pd_job_await(_driver.Handle, _handle, pollMs,
                                                       out var native);
                return ready != 0 ? JobResult.FromNative(in native) : null;
            }, cancellationToken).ConfigureAwait(false);
            if (result is not null)
            {
                return result;
            }
        }
    }

    /// <summary>
    /// Waits for the terminal result, blocking the calling thread.
    /// </summary>
    /// <param name="timeoutMs">How long to wait; 0 waits indefinitely.</param>
    /// <returns>The result, or null on timeout.</returns>
    public JobResult? WaitForResult(uint timeoutMs = 0)
    {
        var ready = NativeMethods.pd_job_await(_driver.Handle, _handle, timeoutMs, out var native);
        return ready != 0 ? JobResult.FromNative(in native) : null;
    }

    internal nint Handle => _handle;

    internal void OnDriverDisposed()
    {
        lock (_gate)
        {
            _driverDisposed = true;
            foreach (var channel in _channels)
            {
                channel.Writer.TryComplete();
            }
            _channels.Clear();
        }
    }

    private ChannelReader<JobEvent> Subscribe()
    {
        var channel = Channel.CreateUnbounded<JobEvent>(new UnboundedChannelOptions
        {
            SingleWriter = false,
            SingleReader = true,
        });
        lock (_gate)
        {
            if (_driverDisposed)
            {
                channel.Writer.TryComplete();
                return channel.Reader;
            }
            _channels.Add(channel);
            if (_subscribed)
            {
                return channel.Reader;
            }
            // First subscriber wins the one native subscription. pd_subscribe_job replays
            // the recorded history synchronously on this thread before returning, so the
            // channel is already primed by the time the caller starts reading and no
            // event can be missed between subscribing and enumerating.
            var context = _driver.Roots.Root(this);
            var callback = new NativeMethods.JobEventCallback(Trampoline);
            _driver.Roots.Root(callback);
            _subscribed = true;
            NativeMethods.pd_subscribe_job(_driver.Handle, _handle, callback,
                                           GCHandle.ToIntPtr(context));
            return channel.Reader;
        }
    }

    private static void Trampoline(nint _, PdJobEvent jobEvent, nint context)
    {
        var job = CallbackRoots.FromContext<PrintJob>(context);
        if (job is null)
        {
            return;
        }
        job.Publish(JobEvent.FromNative(jobEvent));
    }

    private void Publish(JobEvent jobEvent)
    {
        Channel<JobEvent>[] channels;
        lock (_gate)
        {
            channels = [.. _channels];
        }
        foreach (var channel in channels)
        {
            channel.Writer.TryWrite(jobEvent);
            if (jobEvent.IsTerminal)
            {
                channel.Writer.TryComplete();
            }
        }
    }
}
