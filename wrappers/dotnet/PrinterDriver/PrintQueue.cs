using System.Runtime.InteropServices;

namespace PrinterDriver;

/// <summary>In what order a lane's waiting jobs are chosen.</summary>
public enum DrainOrder
{
    /// <summary>Submission order. The safe default for tickets that must stay in sequence.</summary>
    Fifo = 0,

    /// <summary>Higher priority first, submission order within equal priorities.</summary>
    Priority = 1,
}

/// <summary>What the queue does with a job it cannot send yet.</summary>
public sealed class QueuePolicy
{
    /// <summary>
    /// Park jobs while the printer is known to be offline, coverless or out of paper,
    /// instead of failing them one at a time. False makes the queue a pure serializer.
    /// </summary>
    public bool HoldWhileOffline { get; set; } = true;

    /// <summary>
    /// Shelf life for a held job, in milliseconds; 0 means it never expires. A kitchen
    /// ticket must not print into a recovered kitchen half an hour late.
    /// </summary>
    public uint DefaultTtlMs { get; set; }

    /// <summary>
    /// Held jobs per printer. 0 is unlimited, which recreates the printer's own buffer
    /// problem one layer up.
    /// </summary>
    public uint MaxDepth { get; set; } = 64;

    /// <summary>Which waiting job a free lane picks next.</summary>
    public DrainOrder DrainOrder { get; set; } = DrainOrder.Fifo;
}

/// <summary>Per-job queue settings. The printing half mirrors <see cref="JobOptions"/>.</summary>
public sealed class QueueOptions
{
    /// <summary>
    /// The idempotency key. A key that already has a job — held, printing, or finished
    /// months ago — returns that job and prints nothing.
    /// </summary>
    public string? Key { get; set; }

    /// <summary>0 uses the policy default.</summary>
    public uint TtlMs { get; set; }

    /// <summary>Orders the waiting set only. A job already in flight is never preempted.</summary>
    public int Priority { get; set; }

    /// <summary>What the printer's cutter should do at the end of this job.</summary>
    public CutSetting Cut { get; set; } = CutSetting.Profile;

    /// <summary>Kick the cash drawer as part of this job.</summary>
    public bool OpenDrawer { get; set; }

    /// <summary>
    /// Strict refuses the job on cover-open or paper-out before a single payload byte is
    /// written.
    /// </summary>
    public PreflightMode Preflight { get; set; } = PreflightMode.Strict;

    /// <summary>0 uses the profile's completion timeout.</summary>
    public uint TimeoutMs { get; set; }
}

/// <summary>
/// M13b. A policy queue in front of one <see cref="PrinterDriver"/> — the print-queue
/// addon of docs/sdk-spec.md section 12, reached through <c>pd_queue_*</c>.
/// </summary>
/// <remarks>
/// <para>
/// Layered on the public API, never part of it. The core already contains the only queue
/// correctness requires — one active job per printer, with a completion fence between
/// jobs — and that part is not optional and not policy. Everything here is: holding while
/// a printer is unusable, draining on recovery, expiry, priority, depth limits.
/// </para>
/// <para>
/// Three rules from section 12 are load-bearing, and none of them is implemented in C#.
/// They live in the C++ addon behind the ABI, so this behaves identically to the Swift and
/// Dart surfaces. (1) A queue is not a retry engine: a job that ends
/// <see cref="JobOutcome.Unknown"/> blocks its printer's lane until <see cref="Unblock"/>
/// is called by somebody who has looked at the paper. (2) Idempotency keys flow through
/// into the driver's own index. (3) No bypass: draining runs the identical engine path
/// <see cref="Printer.Print"/> takes.
/// </para>
/// <para>Dispose this before the driver it was built on.</para>
/// </remarks>
public sealed class PrintQueue : IDisposable
{
    private readonly PrinterDriver _driver;
    private nint _handle;

    /// <summary>Creates a queue on <paramref name="driver"/>.</summary>
    /// <param name="driver">The driver whose printers this queue feeds.</param>
    /// <param name="policy">Null uses the defaults documented on <see cref="QueuePolicy"/>.</param>
    public PrintQueue(PrinterDriver driver, QueuePolicy? policy = null)
    {
        ArgumentNullException.ThrowIfNull(driver);
        _driver = driver;
        policy ??= new QueuePolicy();
        var native = new PdQueuePolicy
        {
            HoldWhileOffline = policy.HoldWhileOffline ? 1 : 0,
            DefaultTtlMs = policy.DefaultTtlMs,
            MaxDepth = policy.MaxDepth,
            DrainOrder = (int)policy.DrainOrder,
        };
        _handle = NativeMethods.pd_queue_create(driver.Handle, in native);
        if (_handle == 0)
        {
            throw new PrinterDriverException(driver.LastError());
        }
    }

    /// <summary>
    /// Enqueues a job. The returned <see cref="PrintJob"/> is an ordinary job handle —
    /// same id, same event stream — already sent when the printer is usable and its lane
    /// is free, otherwise held, or already terminal with
    /// <see cref="FailureReason.QueueOverflow"/> when the lane is full.
    /// </summary>
    public PrintJob Enqueue(Printer printer, Payload payload, QueueOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(printer);
        ArgumentNullException.ThrowIfNull(payload);
        options ??= new QueueOptions();
        using var marshalled = new PayloadMarshaller(payload);
        var key = Marshal.StringToCoTaskMemUTF8(options.Key ?? string.Empty);
        try
        {
            var native = new PdQueueOptions
            {
                Key = key,
                TtlMs = options.TtlMs,
                Priority = options.Priority,
                Cut = (int)options.Cut,
                OpenDrawer = options.OpenDrawer ? 1 : 0,
                Preflight = (int)options.Preflight,
                TimeoutMs = options.TimeoutMs,
            };
            var payloadStruct = marshalled.Native;
            var job = NativeMethods.pd_queue_enqueue(Handle, printer.Handle, in payloadStruct,
                                                     in native);
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

    /// <summary>Operator hold, independent of what the device is reporting.</summary>
    public void Pause(string printerId) => NativeMethods.pd_queue_pause(Handle, printerId);

    /// <summary>Lifts an operator hold placed by <see cref="Pause"/>.</summary>
    public void Resume(string printerId) => NativeMethods.pd_queue_resume(Handle, printerId);

    /// <summary>Whether an operator hold is in force on this lane.</summary>
    public bool IsPaused(string printerId) =>
        NativeMethods.pd_queue_is_paused(Handle, printerId) != 0;

    /// <summary>
    /// True once a job on this printer ended <see cref="JobOutcome.Unknown"/>. Nothing more
    /// drains onto that lane until a person has looked at the paper and called
    /// <see cref="Unblock"/>.
    /// </summary>
    public bool IsBlocked(string printerId) =>
        NativeMethods.pd_queue_is_blocked(Handle, printerId) != 0;

    /// <summary>
    /// Clears the block left by an unknown outcome. An operator decision, never a timer:
    /// no timer can look at the paper.
    /// </summary>
    public void Unblock(string printerId) => NativeMethods.pd_queue_unblock(Handle, printerId);

    /// <summary>Held jobs. A null or empty id counts every lane.</summary>
    public ulong Pending(string? printerId = null) =>
        (ulong)NativeMethods.pd_queue_pending(Handle, printerId);

    /// <summary>Jobs this queue has ended because their shelf life ran out.</summary>
    public ulong ExpiredCount => (ulong)NativeMethods.pd_queue_expired_count(Handle);

    /// <summary>Jobs refused because a lane was already at its depth limit.</summary>
    public ulong OverflowCount => (ulong)NativeMethods.pd_queue_overflow_count(Handle);

    /// <summary>Jobs this queue has released onto a printer.</summary>
    public ulong DrainedCount => (ulong)NativeMethods.pd_queue_drained_count(Handle);

    /// <summary>
    /// Runs one expiry-and-drain pass on the calling thread. The queue's own thread already
    /// does this on every device event and whenever a TTL comes due.
    /// </summary>
    public void Tick() => NativeMethods.pd_queue_tick(Handle);

    /// <summary>
    /// Stops the queue thread and frees the handle. Held jobs stay held and stay
    /// non-terminal: the queue does not invent an outcome for a job whose fate it does not
    /// know.
    /// </summary>
    public void Dispose()
    {
        var handle = Interlocked.Exchange(ref _handle, 0);
        if (handle != 0)
        {
            NativeMethods.pd_queue_destroy(handle);
        }
    }

    private nint Handle
    {
        get
        {
            var handle = Volatile.Read(ref _handle);
            ObjectDisposedException.ThrowIf(handle == 0, this);
            return handle;
        }
    }
}
