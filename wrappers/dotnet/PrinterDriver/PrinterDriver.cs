using System.Runtime.InteropServices;

namespace PrinterDriver;

/// <summary>
/// The SDK entry point: owns the durable job journal, every printer's worker thread, and
/// every handle this wrapper ever returns.
/// </summary>
/// <remarks>
/// <para>
/// Dispose it once, at application shutdown. <see cref="Dispose"/> stops every printer
/// worker, waits for in-flight jobs to reach a terminal state, and only then releases the
/// callback roots — see <see cref="CallbackRoots"/> for why the order matters.
/// </para>
/// <para>
/// Handles are owned by the driver and are never freed individually, so the same
/// underlying job always maps to the same <see cref="PrintJob"/> instance. Idempotency-key
/// dedupe is therefore visible as reference equality, which is what
/// <c>PrintJobTests</c> asserts.
/// </para>
/// </remarks>
public sealed class PrinterDriver : IDisposable
{
    private readonly object _gate = new();
    private readonly CallbackRoots _roots = new();
    private readonly Dictionary<nint, Printer> _printers = [];
    private readonly Dictionary<nint, PrintJob> _jobs = [];
    private nint _handle;

    private PrinterDriver(nint handle) => _handle = handle;

    /// <summary>Opens a driver.</summary>
    /// <param name="config">Storage and durability settings; null means in-memory.</param>
    /// <returns>The driver. Dispose it at shutdown.</returns>
    /// <exception cref="PrinterDriverException">
    /// The storage directory could not be created. That is the only reason
    /// <c>pd_create</c> can fail, and it reports no detail — there is no handle to hang
    /// one on.
    /// </exception>
    public static PrinterDriver Create(PrinterDriverConfig? config = null)
    {
        config ??= new PrinterDriverConfig();
        var directory = config.StorageDirectory is { Length: > 0 } value
            ? Marshal.StringToCoTaskMemUTF8(value)
            : 0;
        try
        {
            var native = new PdConfig
            {
                StorageDirectory = directory,
                FsyncDisabled = config.FsyncDisabled ? 1 : 0,
                Log = 0,
                LogContext = 0,
            };
            var handle = NativeMethods.pd_create(in native);
            if (handle == 0)
            {
                throw new PrinterDriverException(
                    "the driver could not be created; the usual cause is a storage " +
                    $"directory that cannot be created ('{config.StorageDirectory}')");
            }
            return new PrinterDriver(handle);
        }
        finally
        {
            if (directory != 0)
            {
                Marshal.FreeCoTaskMem(directory);
            }
        }
    }

    /// <summary>
    /// The capability profile ids <see cref="TcpPrinterConfig.ProfileId"/> accepts, read
    /// from the ABI rather than hardcoded.
    /// </summary>
    public static IReadOnlyList<string> ProfileIds
    {
        get
        {
            var array = NativeMethods.pd_profile_ids();
            if (array == 0)
            {
                return [];
            }
            var ids = new List<string>();
            for (var i = 0; ; i++)
            {
                var entry = Marshal.ReadIntPtr(array, i * nint.Size);
                if (entry == 0)
                {
                    break;
                }
                ids.Add(NativeMethods.ReadUtf8(entry));
            }
            return ids;
        }
    }

    /// <summary>Attaches a printer reached over raw TCP (port 9100 by default).</summary>
    /// <param name="config">Where the printer is and what it is.</param>
    /// <returns>The printer, owned by this driver.</returns>
    /// <exception cref="PrinterDriverException">The ABI refused the configuration.</exception>
    public Printer AddPrinterTcp(TcpPrinterConfig config)
    {
        ArgumentNullException.ThrowIfNull(config);
        var handle = Handle;
        var id = Marshal.StringToCoTaskMemUTF8(config.PrinterId ?? string.Empty);
        var host = Marshal.StringToCoTaskMemUTF8(config.Host);
        var profile = Marshal.StringToCoTaskMemUTF8(config.ProfileId ?? string.Empty);
        try
        {
            var native = new PdTcpConfig
            {
                PrinterId = id,
                Host = host,
                Port = config.Port,
                WidthDots = config.WidthDots,
                ProfileId = profile,
                ConnectTimeoutMs = config.ConnectTimeoutMs,
            };
            var printer = NativeMethods.pd_add_printer_tcp(handle, in native);
            if (printer == 0)
            {
                throw new PrinterDriverException(LastError());
            }
            return Intern(printer);
        }
        finally
        {
            Marshal.FreeCoTaskMem(id);
            Marshal.FreeCoTaskMem(host);
            Marshal.FreeCoTaskMem(profile);
        }
    }

    /// <summary>
    /// Any job this driver knows about, including ones reloaded from the journal after a
    /// restart.
    /// </summary>
    /// <param name="key">The idempotency key.</param>
    /// <returns>The job, or null when the key is unknown.</returns>
    public PrintJob? FindJob(string key)
    {
        ArgumentException.ThrowIfNullOrEmpty(key);
        var job = NativeMethods.pd_find_job(Handle, key);
        return job == 0 ? null : InternJob(job);
    }

    /// <summary>
    /// The path the native library was loaded from, for diagnosing a deployment that
    /// bound the wrong build.
    /// </summary>
    public static string? NativeLibraryPath => NativeLibraryResolver.ResolvedPath;

    internal nint Handle
    {
        get
        {
            var handle = Volatile.Read(ref _handle);
            ObjectDisposedException.ThrowIf(handle == 0, this);
            return handle;
        }
    }

    internal CallbackRoots Roots => _roots;

    internal string LastError()
    {
        var handle = Volatile.Read(ref _handle);
        if (handle == 0)
        {
            return "the driver has been disposed";
        }
        var message = NativeMethods.ReadUtf8(NativeMethods.pd_last_error(handle));
        return message.Length == 0 ? "the ABI reported no reason" : message;
    }

    internal Printer Intern(nint printer)
    {
        lock (_gate)
        {
            if (_printers.TryGetValue(printer, out var existing))
            {
                return existing;
            }
            var wrapper = new Printer(this, printer);
            _printers[printer] = wrapper;
            return wrapper;
        }
    }

    /// <summary>
    /// One <see cref="PrintJob"/> per <c>pd_job*</c>, forever. pd.h guarantees the same
    /// underlying job always maps to the same pointer, so interning here is what turns
    /// key dedupe into reference equality on the managed side.
    /// </summary>
    internal PrintJob InternJob(nint job)
    {
        lock (_gate)
        {
            if (_jobs.TryGetValue(job, out var existing))
            {
                return existing;
            }
            var wrapper = new PrintJob(this, job);
            _jobs[job] = wrapper;
            return wrapper;
        }
    }

    /// <summary>
    /// Stops every worker, waits for in-flight jobs to become terminal, then releases the
    /// callback roots.
    /// </summary>
    public void Dispose()
    {
        var handle = Interlocked.Exchange(ref _handle, 0);
        if (handle == 0)
        {
            return;
        }
        // Order matters and is not negotiable: pd_destroy blocks until every worker
        // thread has stopped, which is the moment after which no callback can run. Only
        // then is it safe to drop the delegates those threads were calling.
        NativeMethods.pd_destroy(handle);
        _roots.FreeAll();
        lock (_gate)
        {
            foreach (var job in _jobs.Values)
            {
                job.OnDriverDisposed();
            }
            _jobs.Clear();
            _printers.Clear();
        }
    }
}
