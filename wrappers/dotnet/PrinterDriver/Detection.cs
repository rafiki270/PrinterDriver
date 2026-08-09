using System.Collections.Concurrent;
using System.Runtime.InteropServices;

namespace PrinterDriver;

// M15 — self-test, auto-detection and LAN discovery (docs/api.md §15).
//
// The wrapper contains no detection logic. Which provenance column governs a mechanism,
// what a printless probe may claim, how a ticket is laid out — all of it is in the core,
// behind pd_self_test, pd_auto_detect and pd_discover. What is here is the shape a .NET
// caller expects: records, and IAsyncEnumerable for the two calls that deliver candidates
// as they are found.

/// <summary>
/// How the capability profile in force was arrived at — <c>pd_profile_selection</c>.
/// </summary>
/// <remarks>
/// Not a <see cref="Provenance"/>: that says where a claim about one <em>capability</em>
/// comes from, and this says where the <em>profile</em> came from.
/// </remarks>
public enum ProfileSelection
{
    /// <summary>A device-database entry matched what the device reported about itself.</summary>
    Documented = 0,

    /// <summary>A probe's first-hand findings promoted whatever was selected.</summary>
    Probed = 1,

    /// <summary>
    /// Neither: the shipped default is the whole truth, which means UNKNOWN DEVICE rather
    /// than ordinary device.
    /// </summary>
    Default = 2,
}

/// <summary>What auto-detection established about one address — <c>pd_detection_status</c>.</summary>
public enum DetectionStatus
{
    /// <summary>The backchannel answered: identification, fences, or both.</summary>
    Answered = 0,

    /// <summary>
    /// The port accepted the connection and said nothing at all. A real finding — the
    /// interface that does not forward status bytes — and never a failure.
    /// </summary>
    Silent = 1,

    /// <summary>
    /// Reachable and deliberately not interrogated. Never render this as "no
    /// capabilities": nobody asked.
    /// </summary>
    Unverified = 2,

    /// <summary>The connection was refused, timed out, or the port is closed.</summary>
    Unreachable = 3,
}

/// <summary>What a device said about itself, and what that is worth.</summary>
/// <remarks>
/// The whole record is evidence, never truth. <see cref="IsTrusted"/> is false until a
/// signal independent of <c>GS I</c> agrees with <c>GS I</c>, because at least one family
/// ships answering as somebody else's model.
/// </remarks>
public sealed record DetectedIdentity(
    string Vendor,
    string Model,
    string Firmware,
    string Serial,
    bool IsTrusted,
    byte ConfidencePercent,
    bool IsImpersonationSuspected,
    bool IsFresh);

/// <summary>
/// The media facts the renderer consumes. Roll width and raster width are separate facts
/// and neither is derived from the other.
/// </summary>
public sealed record DetectedMedia(
    ushort NominalPaperMm,
    uint PrintableWidthDots,
    uint CharsPerLine,
    ushort Dpi);

/// <summary>
/// The mechanism, the best grade a job on it can ever claim, who makes that claim, and
/// what the claim rests on.
/// </summary>
public sealed record DetectedCompletion(
    CompletionMechanism Mechanism,
    ConfidenceGrade GradeCeiling,
    CompletionAuthority Authority,
    string Method,
    Provenance Provenance);

/// <summary>The drawer facet, classified rather than fired (docs/cash-drawer.md).</summary>
public sealed record DetectedDrawer(
    bool IsPresent,
    bool IsKickable,
    DrawerPortStandard PortStandard,
    ushort Voltage,
    Provenance ElectricalProvenance,
    Provenance CommandsProvenance);

/// <summary>The detection report — <c>pd_detection_summary</c>.</summary>
public sealed record DetectionSummary(
    string Endpoint,
    DetectedIdentity Identity,
    string ProfileId,
    ProfileSelection Selection,
    DetectedMedia Media,
    DetectedCompletion Completion,
    DetectedDrawer Drawer,
    IReadOnlyList<string> Degradations,
    string ProvenanceSummary)
{
    internal static DetectionSummary FromNative(in PdDetectionSummary native)
    {
        var degradations = new List<string>();
        if (native.Degradations != 0)
        {
            for (nuint index = 0; index < native.DegradationCount; index++)
            {
                var entry = Marshal.ReadIntPtr(
                    native.Degradations, checked((int)index) * nint.Size);
                degradations.Add(NativeMethods.ReadUtf8(entry));
            }
        }

        return new DetectionSummary(
            NativeMethods.ReadUtf8(native.Endpoint),
            new DetectedIdentity(
                NativeMethods.ReadUtf8(native.Vendor),
                NativeMethods.ReadUtf8(native.Model),
                NativeMethods.ReadUtf8(native.Firmware),
                NativeMethods.ReadUtf8(native.Serial),
                native.IdentityTrusted != 0,
                native.ConfidencePercent,
                native.ImpersonationSuspected != 0,
                native.IdentityFresh != 0),
            NativeMethods.ReadUtf8(native.ProfileId),
            (ProfileSelection)native.Selection,
            new DetectedMedia(
                native.NominalPaperMm, native.PrintableWidthDots, native.CharsPerLine,
                native.Dpi),
            new DetectedCompletion(
                (CompletionMechanism)native.Completion,
                (ConfidenceGrade)native.GradeCeiling,
                (CompletionAuthority)native.Authority,
                NativeMethods.ReadUtf8(native.Method),
                (Provenance)native.CompletionProvenance),
            new DetectedDrawer(
                native.DrawerPresent != 0,
                native.DrawerKickable != 0,
                (DrawerPortStandard)native.DrawerStandard,
                native.DrawerVoltage,
                (Provenance)native.DrawerElectricalProvenance,
                (Provenance)native.DrawerCommandsProvenance),
            degradations,
            NativeMethods.ReadUtf8(native.ProvenanceSummary));
    }
}

/// <summary>One candidate, classified — <c>pd_detected_printer</c>.</summary>
public sealed record DetectedPrinter(
    string Endpoint,
    string Host,
    ushort Port,
    DetectionStatus Status,
    bool IsPortOpen,
    bool IsFromCache,
    string DleEotHex,
    DetectionSummary Summary)
{
    internal static DetectedPrinter FromNative(in PdDetectedPrinter native) =>
        new(NativeMethods.ReadUtf8(native.Endpoint),
            NativeMethods.ReadUtf8(native.Host),
            native.Port,
            (DetectionStatus)native.Status,
            native.PortOpen != 0,
            native.FromCache != 0,
            NativeMethods.ReadUtf8(native.DleEotHex),
            DetectionSummary.FromNative(native.Summary));
}

/// <summary>One address the LAN sweep found listening — <c>pd_discovered_device</c>.</summary>
public sealed record DiscoveredDevice(
    string Ip,
    ushort Port,
    bool IsPortOpen,
    string DleEotHex)
{
    /// <summary>Whether anything came back on the backchannel at all.</summary>
    public bool DidAnswer => DleEotHex.Length != 0;

    internal static DiscoveredDevice FromNative(in PdDiscoveredDevice native) =>
        new(NativeMethods.ReadUtf8(native.Ip),
            native.Port,
            native.Port9100Open != 0,
            NativeMethods.ReadUtf8(native.DleEotHex));
}

/// <summary>Options for <see cref="Printer.SelfTest"/>. All-defaults is the useful call.</summary>
public sealed record SelfTestOptions
{
    /// <summary>
    /// <c>null</c> produces <c>selftest-&lt;unix ms&gt;</c>. A real idempotency key on a
    /// real job: the same key twice does not print twice.
    /// </summary>
    public string? Key { get; init; }

    /// <summary>Interrogate the device now instead of using what is already known.</summary>
    public bool RefreshIdentity { get; init; }

    /// <summary>
    /// Keep that refresh printless, at the cost of asking the ordered fences out of an
    /// empty buffer.
    /// </summary>
    public bool ProbeWithoutPrinting { get; init; }

    /// <summary>
    /// Include the Code 128 sample. A profile with no barcode path omits it anyway, with
    /// a declared degradation printed on the ticket.
    /// </summary>
    public bool Barcode { get; init; } = true;

    /// <summary><c>null</c> uses the core's own sample, <c>PD-SELFTEST</c>.</summary>
    public string? BarcodeData { get; init; }

    /// <summary>
    /// Print the trailer <c>V:</c> line and its QR. On a <c>GS ( H</c> printer that QR
    /// carries the job's own verification token, which is what makes the paper evidence.
    /// </summary>
    public bool PrintVerificationId { get; init; } = true;

    /// <summary>0 means the profile's completion budget.</summary>
    public uint TimeoutMs { get; init; }
}

/// <summary>What one diagnostic ticket established.</summary>
/// <remarks>
/// <see cref="Result"/> is the proof: the ordinary tri-state outcome of the ordinary
/// engine, so a <see cref="JobResult.Done"/> at
/// <see cref="ConfidenceGrade.AJobLevelConfirmation"/> is the statement that this stack
/// works end to end on this unit.
/// </remarks>
public sealed record SelfTestResult(
    JobResult Result,
    DetectionSummary Detection,
    string Key,
    string? VerificationId,
    IReadOnlyList<string> TicketLines,
    PrintJob? Job);

/// <summary>Options for <see cref="PrinterDriver.AutoDetect"/>.</summary>
public sealed record AutoDetectOptions
{
    /// <summary><c>null</c> sweeps the local /24. Ignored when <see cref="Endpoints"/> is set.</summary>
    public string? SubnetCidr { get; init; }

    /// <summary>
    /// An explicit <c>host</c> / <c>host:port</c> list that skips the sweep entirely — the
    /// path for a caller with a known inventory, and the only one that reports an
    /// unreachable address, because it is the only one where somebody named it.
    /// </summary>
    public IReadOnlyList<string> Endpoints { get; init; } = Array.Empty<string>();

    /// <summary>0 selects the ABI default, port 9100.</summary>
    public ushort Port { get; init; }

    /// <summary>Sockets in flight at once. 0 selects the ABI default.</summary>
    public uint Concurrency { get; init; }

    /// <summary>How long one connect may take. 0 selects the ABI default.</summary>
    public uint ConnectTimeoutMs { get; init; }

    /// <summary>
    /// How long to wait for the <c>DLE EOT 1</c> answer once the port is open. Silence is
    /// a finding, so this budget is short on purpose. 0 selects the ABI default.
    /// </summary>
    public uint ResponseTimeoutMs { get; init; }

    /// <summary>
    /// False leaves devices nobody has interrogated alone: cached findings still apply and
    /// anything untouched comes back <see cref="DetectionStatus.Unverified"/>.
    /// </summary>
    public bool ProbeUnknown { get; init; } = true;
}

/// <summary>Options for <see cref="PrinterDriver.Discover"/>.</summary>
public sealed record DiscoverOptions
{
    /// <summary><c>null</c> sweeps the local /24.</summary>
    public string? SubnetCidr { get; init; }

    /// <summary>0 selects the ABI default, port 9100.</summary>
    public ushort Port { get; init; }

    /// <summary>Sockets in flight at once. 0 selects the ABI default.</summary>
    public uint Concurrency { get; init; }

    /// <summary>How long one connect may take. 0 selects the ABI default.</summary>
    public uint ConnectTimeoutMs { get; init; }

    /// <summary>
    /// How long to wait for the <c>DLE EOT 1</c> answer once the port is open. Silence is
    /// a finding, so this budget is short on purpose. 0 selects the ABI default.
    /// </summary>
    public uint ResponseTimeoutMs { get; init; }

    /// <summary>
    /// False turns the sweep into a pure port scan: the port state is still reported and
    /// not one byte is written.
    /// </summary>
    public bool ProbeBackchannel { get; init; } = true;
}

public sealed partial class Printer
{
    /// <summary>
    /// Prints ONE diagnostic ticket through the full fenced engine and reports what it
    /// established. <b>This uses paper: the paper is the report.</b>
    /// </summary>
    /// <remarks>
    /// <para>
    /// Identity, profile and how it was selected, media, completion mechanism with its
    /// grade ceiling and provenance, the drawer classification, a Czech/Hungarian/Polish
    /// charset line, a Code 128 sample and the job's own verification token in the trailer
    /// QR. Anything the profile cannot draw is printed as a declared degradation rather
    /// than dropped, and repeated in <see cref="DetectionSummary.Degradations"/>.
    /// </para>
    /// <para>
    /// The printer's OWN built-in self-test (<c>GS ( A</c>) is a different document and
    /// stays separately reachable through <c>pdctl test-print</c>.
    /// </para>
    /// <para>Blocks until the job is terminal.</para>
    /// </remarks>
    public SelfTestResult SelfTest(SelfTestOptions? options = null)
    {
        options ??= new SelfTestOptions();
        var key = Utf8Buffer.From(options.Key);
        var barcodeData = Utf8Buffer.From(options.BarcodeData);
        try
        {
            var native = new PdSelfTestOptions
            {
                Key = key.Pointer,
                RefreshIdentity = options.RefreshIdentity ? 1 : 0,
                ProbeWithoutPrinting = options.ProbeWithoutPrinting ? 1 : 0,
                NoBarcode = options.Barcode ? 0 : 1,
                BarcodeData = barcodeData.Pointer,
                NoVerificationId = options.PrintVerificationId ? 0 : 1,
                TimeoutMs = options.TimeoutMs,
            };
            if (NativeMethods.pd_self_test(_driver.Handle, _handle, in native, out var result) == 0)
            {
                throw new PrinterDriverException(_driver.LastError());
            }

            var token = NativeMethods.ReadUtf8(result.PrintToken);
            var ticket = NativeMethods.ReadUtf8(result.TicketText);
            return new SelfTestResult(
                JobResult.FromNative(result.Result),
                DetectionSummary.FromNative(result.Detection),
                NativeMethods.ReadUtf8(result.Key),
                token.Length == 0 ? null : token,
                ticket.Length == 0
                    ? Array.Empty<string>()
                    : ticket.Split('\n'),
                result.Job == 0 ? null : _driver.InternJob(result.Job));
        }
        finally
        {
            key.Dispose();
            barcodeData.Dispose();
        }
    }
}

public sealed partial class PrinterDriver
{
    /// <summary>
    /// Sweeps, identifies and classifies. <b>Nothing prints and nothing fires.</b>
    /// </summary>
    /// <remarks>
    /// <para>
    /// Discovery (the non-printing <c>DLE EOT 1</c> sweep) then multi-signal
    /// identification per candidate, then the PRINTLESS subset of the capability probe,
    /// respecting the stored findings cache.
    /// </para>
    /// <para>
    /// An ordered fence only means anything when there is print data ahead of it. This
    /// call has none, so the fences go out behind an empty buffer: a device that echoes
    /// them has proved that its firmware <em>implements</em> the command, not that the
    /// echo waits for paper to move. The flag is promoted and its provenance is not, and
    /// the reason appears in <see cref="DetectionSummary.Degradations"/>. Full promotion
    /// needs the printing probe or a real job.
    /// </para>
    /// <para>Blocks until every candidate is finished.</para>
    /// </remarks>
    public IReadOnlyList<DetectedPrinter> AutoDetect(
        AutoDetectOptions? options = null, Action<DetectedPrinter>? onCandidate = null)
    {
        options ??= new AutoDetectOptions();
        var found = new List<DetectedPrinter>();
        // The callback runs on sweep worker threads, so the list needs a lock even though
        // the core serialises the calls: the memory model, not the ordering, is the issue.
        var gate = new object();

        NativeMethods.DetectedCallback callback = (printer, _, _, _) =>
        {
            var one = DetectedPrinter.FromNative(Marshal.PtrToStructure<PdDetectedPrinter>(printer));
            lock (gate)
            {
                found.Add(one);
            }
            onCandidate?.Invoke(one);
        };

        var cidr = Utf8Buffer.From(options.SubnetCidr);
        var endpoints = Utf8StringArray.From(options.Endpoints);
        try
        {
            var native = new PdAutoDetectOptions
            {
                SubnetCidr = cidr.Pointer,
                Endpoints = endpoints.Pointer,
                Port = options.Port,
                Concurrency = options.Concurrency,
                ConnectTimeoutMs = options.ConnectTimeoutMs,
                ResponseTimeoutMs = options.ResponseTimeoutMs,
                LeaveUnknownUnprobed = options.ProbeUnknown ? 0 : 1,
            };
            var count = NativeMethods.pd_auto_detect(Handle, in native, callback, 0);
            GC.KeepAlive(callback);
            if (count < 0)
            {
                throw new PrinterDriverException(LastError());
            }
        }
        finally
        {
            cidr.Dispose();
            endpoints.Dispose();
        }
        return found;
    }

    /// <summary><see cref="AutoDetect"/> with candidates delivered as they are found.</summary>
    public async IAsyncEnumerable<DetectedPrinter> AutoDetectAsync(
        AutoDetectOptions? options = null,
        [System.Runtime.CompilerServices.EnumeratorCancellation]
        CancellationToken cancellationToken = default)
    {
        var queue = new BlockingCollection<DetectedPrinter>();
        var sweep = Task.Run(() =>
        {
            try
            {
                AutoDetect(options, queue.Add);
            }
            finally
            {
                queue.CompleteAdding();
            }
        }, cancellationToken);

        foreach (var candidate in queue.GetConsumingEnumerable(cancellationToken))
        {
            yield return candidate;
        }
        await sweep.ConfigureAwait(false);
    }

    /// <summary>
    /// The raw sweep underneath <see cref="AutoDetect"/>: is something ESC/POS-shaped
    /// listening, and does its backchannel reach me?
    /// </summary>
    /// <remarks>
    /// No identification, no capability probe, no profile — deciding what a device
    /// <em>is</em> costs time and belongs to <see cref="AutoDetect"/>. The whole write
    /// side is <c>DLE EOT 1</c> (<c>10 04 01</c>), every byte of which is below 0x20 and
    /// therefore cannot print on any device, ever.
    /// </remarks>
    public IReadOnlyList<DiscoveredDevice> Discover(
        DiscoverOptions? options = null, Action<DiscoveredDevice>? onDevice = null)
    {
        options ??= new DiscoverOptions();
        var found = new List<DiscoveredDevice>();
        var gate = new object();

        NativeMethods.DiscoveredCallback callback = (device, _, _, _) =>
        {
            var one = DiscoveredDevice.FromNative(Marshal.PtrToStructure<PdDiscoveredDevice>(device));
            lock (gate)
            {
                found.Add(one);
            }
            onDevice?.Invoke(one);
        };

        var cidr = Utf8Buffer.From(options.SubnetCidr);
        try
        {
            var native = new PdDiscoverOptions
            {
                SubnetCidr = cidr.Pointer,
                Port = options.Port,
                Concurrency = options.Concurrency,
                ConnectTimeoutMs = options.ConnectTimeoutMs,
                ResponseTimeoutMs = options.ResponseTimeoutMs,
                NoBackchannelProbe = options.ProbeBackchannel ? 0 : 1,
            };
            var count = NativeMethods.pd_discover(Handle, in native, callback, 0);
            GC.KeepAlive(callback);
            if (count < 0)
            {
                throw new PrinterDriverException(LastError());
            }
        }
        finally
        {
            cidr.Dispose();
        }
        return found;
    }

    /// <summary><see cref="Discover"/> with listeners delivered as they are found.</summary>
    public async IAsyncEnumerable<DiscoveredDevice> DiscoverAsync(
        DiscoverOptions? options = null,
        [System.Runtime.CompilerServices.EnumeratorCancellation]
        CancellationToken cancellationToken = default)
    {
        var queue = new BlockingCollection<DiscoveredDevice>();
        var sweep = Task.Run(() =>
        {
            try
            {
                Discover(options, queue.Add);
            }
            finally
            {
                queue.CompleteAdding();
            }
        }, cancellationToken);

        foreach (var device in queue.GetConsumingEnumerable(cancellationToken))
        {
            yield return device;
        }
        await sweep.ConfigureAwait(false);
    }

    /// <summary>
    /// The /24 around this host's primary address, as <c>192.168.1.0/24</c>, or
    /// <c>null</c> when it cannot be determined.
    /// </summary>
    /// <remarks>
    /// Found by asking the routing table which local address would be used to reach a
    /// remote one; no packet is transmitted.
    /// </remarks>
    public string? LocalSubnet
    {
        get
        {
            var value = NativeMethods.ReadUtf8(NativeMethods.pd_local_subnet(Handle));
            return value.Length == 0 ? null : value;
        }
    }
}

/// <summary>
/// A NUL-terminated UTF-8 copy of a string, alive until disposed.
/// </summary>
/// <remarks>
/// pd.h copies every <c>const char*</c> it is passed before returning, so the buffer only
/// has to outlive one call — but it must genuinely outlive it, which a stack-allocated
/// span inside a nested lambda does not.
/// </remarks>
internal readonly struct Utf8Buffer : IDisposable
{
    private Utf8Buffer(nint pointer) => Pointer = pointer;

    internal nint Pointer { get; }

    internal static Utf8Buffer From(string? value) =>
        new(value is null ? 0 : Marshal.StringToCoTaskMemUTF8(value));

    public void Dispose()
    {
        if (Pointer != 0)
        {
            Marshal.FreeCoTaskMem(Pointer);
        }
    }
}

/// <summary>A NULL-terminated <c>char*</c> array, alive until disposed.</summary>
internal readonly struct Utf8StringArray : IDisposable
{
    private Utf8StringArray(nint pointer, nint[] entries)
    {
        Pointer = pointer;
        _entries = entries;
    }

    private readonly nint[] _entries;

    internal nint Pointer { get; }

    internal static Utf8StringArray From(IReadOnlyList<string> values)
    {
        if (values.Count == 0)
        {
            return new Utf8StringArray(0, Array.Empty<nint>());
        }
        var entries = new nint[values.Count];
        for (var index = 0; index < values.Count; index++)
        {
            entries[index] = Marshal.StringToCoTaskMemUTF8(values[index]);
        }
        var array = Marshal.AllocCoTaskMem((values.Count + 1) * nint.Size);
        for (var index = 0; index < values.Count; index++)
        {
            Marshal.WriteIntPtr(array, index * nint.Size, entries[index]);
        }
        Marshal.WriteIntPtr(array, values.Count * nint.Size, 0);
        return new Utf8StringArray(array, entries);
    }

    public void Dispose()
    {
        foreach (var entry in _entries)
        {
            Marshal.FreeCoTaskMem(entry);
        }
        if (Pointer != 0)
        {
            Marshal.FreeCoTaskMem(Pointer);
        }
    }
}
