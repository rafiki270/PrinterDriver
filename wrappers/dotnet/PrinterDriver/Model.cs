namespace PrinterDriver;

/// <summary>One step of a job's history.</summary>
/// <param name="State">The state the job entered.</param>
/// <param name="Confidence">What that state rests on.</param>
/// <param name="Reason">
/// The failure cause, or null for every non-failure transition. Null rather than
/// <see cref="FailureReason.None"/> so that "there is no reason here" and "the reason is
/// None" cannot be confused.
/// </param>
/// <param name="MonotonicMs">
/// Milliseconds from a steady clock, not the wall clock: the wall clock may step and job
/// timing must not.
/// </param>
public sealed record JobEvent(
    JobState State,
    ConfidenceLevel Confidence,
    FailureReason? Reason,
    ulong MonotonicMs)
{
    /// <summary>True when this is the last event the job will ever emit.</summary>
    public bool IsTerminal => State is JobState.DoneSoftware or JobState.PhysicallyVerified
        or JobState.FailedKnown or JobState.Unknown;

    internal static JobEvent FromNative(in PdJobEvent native) => new(
        (JobState)native.State,
        (ConfidenceLevel)native.Confidence,
        native.HasReason != 0 ? (FailureReason)native.Reason : null,
        native.MonotonicMs);
}

/// <summary>
/// Last known device state. Never a live query, so it cannot block behind a print; call
/// <see cref="Printer.RefreshStatus"/> for one that is.
/// </summary>
/// <param name="Connected">Whether the transport is up.</param>
/// <param name="Observed">
/// False until a status frame has actually been decoded. A snapshot that has never heard
/// from the device says so rather than reporting healthy, which is why every field below
/// is nullable.
/// </param>
/// <param name="Online">Printer online, or null when unknown.</param>
/// <param name="CoverOpen">Cover open, or null when unknown.</param>
/// <param name="PaperOut">Paper exhausted, or null when unknown.</param>
/// <param name="PaperNearEnd">Paper near end, or null when unknown.</param>
/// <param name="CutterError">Cutter fault, or null when unknown.</param>
/// <param name="UnrecoverableError">Unrecoverable fault, or null when unknown.</param>
/// <param name="RecoverableError">Recoverable fault, or null when unknown.</param>
public sealed record DeviceStatus(
    bool Connected,
    bool Observed,
    bool? Online,
    bool? CoverOpen,
    bool? PaperOut,
    bool? PaperNearEnd,
    bool? CutterError,
    bool? UnrecoverableError,
    bool? RecoverableError)
{
    // pd.h's tri-state: PD_UNKNOWN (-1), PD_FALSE (0), PD_TRUE (1).
    private static bool? Tri(int value) => value < 0 ? null : value != 0;

    internal static DeviceStatus FromNative(in PdDeviceStatus native) => new(
        native.Connected != 0,
        native.Observed != 0,
        Tri(native.Online),
        Tri(native.CoverOpen),
        Tri(native.PaperOut),
        Tri(native.PaperNearEnd),
        Tri(native.CutterError),
        Tri(native.UnrecoverableError),
        Tri(native.RecoverableError));
}

/// <summary>How a driver stores its journal.</summary>
/// <param name="StorageDirectory">
/// Where the durable job journal lives. Null or empty means in-memory: no journal, no
/// crash recovery. Never appropriate for kitchen tickets.
/// </param>
/// <param name="FsyncDisabled">
/// Makes journal writes far faster and the durability guarantee worthless. For tests
/// only.
/// </param>
public sealed record PrinterDriverConfig(
    string? StorageDirectory = null,
    bool FsyncDisabled = false);

/// <summary>A network printer to attach to a driver.</summary>
/// <param name="Host">Host name or address. Required.</param>
/// <param name="Port">TCP port; 0 means the raw-printing default, 9100.</param>
/// <param name="PrinterId">Stable identifier; null derives one from the endpoint.</param>
/// <param name="WidthDots">Print width in dots; 0 means 576. 384, 504 and 576 are deployed.</param>
/// <param name="ProfileId">
/// A capability profile from <see cref="PrinterDriver.ProfileIds"/>; null means "generic".
/// </param>
/// <param name="ConnectTimeoutMs">Connect timeout; 0 means 3000.</param>
public sealed record TcpPrinterConfig(
    string Host,
    ushort Port = 0,
    string? PrinterId = null,
    uint WidthDots = 0,
    string? ProfileId = null,
    uint ConnectTimeoutMs = 0);

/// <summary>Per-job options.</summary>
/// <param name="Key">
/// The idempotency key. Null or empty gets a generated one and therefore no dedupe
/// protection: re-submitting prints a second copy. Pass a real key for anything a
/// customer would notice twice.
/// </param>
/// <param name="Cut">What to do with the paper at the end.</param>
/// <param name="OpenDrawer">Kick the cash drawer when the job completes.</param>
/// <param name="Preflight">Whether to check the device before sending.</param>
/// <param name="TimeoutMs">Completion timeout; 0 means the profile's own.</param>
public sealed record JobOptions(
    string? Key = null,
    CutSetting Cut = CutSetting.Profile,
    bool OpenDrawer = false,
    PreflightMode Preflight = PreflightMode.Strict,
    uint TimeoutMs = 0);

/// <summary>Anything the SDK refuses to do, with the C ABI's own explanation attached.</summary>
public sealed class PrinterDriverException : Exception
{
    /// <summary>Creates the exception.</summary>
    /// <param name="message">What went wrong.</param>
    public PrinterDriverException(string message) : base(message) { }

    /// <summary>Creates the exception.</summary>
    /// <param name="message">What went wrong.</param>
    /// <param name="inner">The underlying failure.</param>
    public PrinterDriverException(string message, Exception inner) : base(message, inner) { }
}
