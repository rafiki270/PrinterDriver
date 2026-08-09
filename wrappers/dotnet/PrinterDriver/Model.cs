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

/// <summary>A printer reached over a link the caller owns — see <see cref="IPrinterTransport"/>.</summary>
/// <param name="Description">
/// What the printer id and the diagnostics derive from, e.g.
/// <c>"bt-spp:00:11:22:33:44:55"</c>. Null or empty becomes <c>"custom"</c>, which is fine
/// for one printer and ambiguous for two.
/// </param>
/// <param name="ProfileId">
/// A capability profile from <see cref="PrinterDriver.ProfileIds"/>; null means "generic".
/// An unknown id is refused rather than silently downgraded: a caller that asked for a
/// TM-T88VI and got the unknown-device profile would be told a weaker completion story
/// than it asked for, with nothing in the result explaining why.
/// </param>
/// <param name="WidthDots">Print width in dots; 0 means 576.</param>
public sealed record CustomPrinterConfig(
    string? Description = null,
    string? ProfileId = null,
    uint WidthDots = 0);

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

// --- M14: cash drawer (docs/cash-drawer.md) -------------------------------------------

/// <summary>
/// What a printer's drawer port is, and what is known about it.
/// </summary>
/// <remarks>
/// A separate peripheral facet, not part of the printer's own capabilities: the connector
/// pinout, the drive voltage and the command the firmware implements are three independent
/// facts, and none of them follows from anything the printer does with paper.
/// </remarks>
/// <param name="Present">This model has a drawer port at all.</param>
/// <param name="PortStandard">
/// The electrical classification. Nothing is ever fired on
/// <see cref="DrawerPortStandard.Unknown"/>.
/// </param>
/// <param name="Voltage">Volts, or 0 where the manufacturer does not document it — which is not "low".</param>
/// <param name="MaxCurrentMa">Milliamps, or 0 where undocumented.</param>
/// <param name="ChannelCount">Drive outputs, typically 2.</param>
/// <param name="SensorPin">3 on the Epson arrangement, 6 on Star's; 0 when unestablished.</param>
/// <param name="KickMethod">Which software path fires it.</param>
/// <param name="DefaultPulseMs">The profile's own pulse, 200 ms on the documented 24 V families.</param>
/// <param name="MaxPulseMs">The ceiling a request is clamped to.</param>
/// <param name="CooldownMs">Held between two pulses so a retry loop cannot keep a solenoid energised.</param>
/// <param name="CanKickDuringPrint">
/// False on models whose drawer output cannot fire while the mechanism prints; the pulse is
/// then ordered strictly behind everything already queued.
/// </param>
/// <param name="StatusAvailable">There is a readable switch.</param>
/// <param name="StatusMethod">How it is read.</param>
/// <param name="SharedBetweenDrawers">
/// Two drive outputs and one switch input: both channels kick independently while the only
/// readable fact is that some attached drawer is open.
/// </param>
/// <param name="SharedWithBuzzer">
/// Epson documents that with the optional external buzzer enabled the pulse sounds the
/// buzzer instead of firing the drawer. Never assume both coexist.
/// </param>
/// <param name="ElectricalProvenance">
/// Deliberately separate from <paramref name="CommandsProvenance"/>: the XP-S260M's
/// DC 24 V / 1 A output is in Xprinter's own specification while nothing they publish
/// proves the pulse command.
/// </param>
/// <param name="CommandsProvenance">Where the claim about the command set comes from.</param>
/// <param name="Kickable">
/// Whether this SDK may put a pulse on the wire: a method it can drive and an established
/// electrical standard. A caller that reads nothing else should read this.
/// </param>
public sealed record DrawerCapabilities(
    bool Present,
    DrawerPortStandard PortStandard,
    ushort Voltage,
    ushort MaxCurrentMa,
    byte ChannelCount,
    byte SensorPin,
    DrawerKickMethod KickMethod,
    ushort DefaultPulseMs,
    ushort MaxPulseMs,
    ushort CooldownMs,
    bool CanKickDuringPrint,
    bool StatusAvailable,
    DrawerStatusMethod StatusMethod,
    bool SharedBetweenDrawers,
    bool SharedWithBuzzer,
    Provenance ElectricalProvenance,
    Provenance CommandsProvenance,
    bool Kickable)
{
    internal static DrawerCapabilities FromNative(in PdDrawerCapabilities native) =>
        new(
            native.Present == 1,
            (DrawerPortStandard)native.Standard,
            native.Voltage,
            native.MaxCurrentMa,
            native.ChannelCount,
            native.SensorPin,
            (DrawerKickMethod)native.Method,
            native.DefaultPulseMs,
            native.MaxPulseMs,
            native.CooldownMs,
            native.CanKickDuringPrint == 1,
            native.StatusAvailable == 1,
            (DrawerStatusMethod)native.StatusMethod,
            native.SharedBetweenDrawers == 1,
            native.SharedWithBuzzer == 1,
            (Provenance)native.ElectricalProvenance,
            (Provenance)native.CommandsProvenance,
            native.Kickable == 1);
}

/// <summary>The outcome of one opening sequence — a state, never a boolean.</summary>
/// <param name="State">What was established.</param>
/// <param name="PreviousState">What the switch said before the pulse.</param>
/// <param name="Channel">The output that was energised.</param>
/// <param name="PulseMs">
/// 0 when no pulse was emitted at all, which happens both when the drawer was already open
/// and when the call was refused.
/// </param>
/// <param name="ElapsedMs">
/// The interval between the pulse leaving for the link and the verdict — the "sensor
/// changed 143 ms after kick" number. 0 when there was nothing to wait for.
/// </param>
public sealed record DrawerResult(
    DrawerState State,
    DrawerState PreviousState,
    byte Channel,
    ushort PulseMs,
    uint ElapsedMs)
{
    /// <summary>The one thing worth branching on: the switch was seen changing.</summary>
    public bool Verified => State == DrawerState.OpenVerified;

    internal static DrawerResult FromNative(in PdDrawerResult native) =>
        new((DrawerState)native.State, (DrawerState)native.PreviousState, native.Channel,
            native.PulseMs, native.ElapsedMs);
}

/// <summary>One non-destructive read of the drawer switch.</summary>
/// <param name="Available">The profile documents a readable switch on this port.</param>
/// <param name="Answered">The device actually replied within the timeout.</param>
/// <param name="PinHigh">The raw sense level, or null when nothing answered.</param>
/// <param name="NeedsCalibration">
/// True until this printer's polarity has been measured. While it is true
/// <paramref name="State"/> stays <see cref="DrawerState.Unknown"/> however clear the
/// level is: whether the line reads high or low when the drawer is open depends on the
/// drawer that is plugged in, so this SDK measures it once instead of assuming.
/// </param>
/// <param name="State">The interpretation, where there is one.</param>
public sealed record DrawerReading(
    bool Available,
    bool Answered,
    bool? PinHigh,
    bool NeedsCalibration,
    DrawerState State)
{
    internal static DrawerReading FromNative(in PdDrawerReading native) =>
        new(native.Available == 1,
            native.Answered == 1,
            native.PinHigh switch { 1 => true, 0 => false, _ => null },
            native.NeedsCalibration == 1,
            (DrawerState)native.State);
}
