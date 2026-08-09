namespace PrinterDriver;

// Closed enums, mirrored one-for-one from capi/include/printerdriver/pd.h, which mirrors
// core/include/printerdriver one-for-one in turn (docs/api.md §1.3).
//
// The trailing PD_*_COUNT sentinel of each C enum is deliberately NOT reproduced here: it
// is a member count, not a value a printer can ever be in, and an enum with a bogus
// terminal member is exactly how a `default:` arm ends up shipping. The count is instead
// asserted by PrinterDriver.Tests/EnumBridgeTests.cs, which asks the native enum bridge
// (capi/tests/pd_test_support.h) how many members the C++ core has and how each one is
// spelled, and compares that with what is declared below. A member added to the core
// therefore fails a test here rather than reaching an application as a silent hole.

/// <summary>Where a job is on the evidence ladder (docs/sdk-spec.md §5).</summary>
public enum JobState
{
    /// <summary>Accepted and journalled; nothing has been sent.</summary>
    Queued = 0,

    /// <summary>The device answered a health query and is fit to print.</summary>
    PreflightOk = 1,

    /// <summary>Durably recorded as started; the first payload byte may have gone out.</summary>
    SendStarted = 2,

    /// <summary>Every payload byte was accepted by the link.</summary>
    BytesSent = 3,

    /// <summary>The printer answered the completion fence for the print itself.</summary>
    PrintConfirmed = 4,

    /// <summary>The printer answered the fence placed after the cut command.</summary>
    CutCommandProcessed = 5,

    /// <summary>Terminal: the SDK's strongest software-side claim.</summary>
    DoneSoftware = 6,

    /// <summary>Terminal: confirmed by something outside the SDK.</summary>
    PhysicallyVerified = 7,

    /// <summary>Terminal: it failed, and the cause is known.</summary>
    FailedKnown = 8,

    /// <summary>Terminal, and the only honest answer: it may or may not have printed.</summary>
    Unknown = 9,

    /// <summary>Produced only by the print-queue addon (docs/sdk-spec.md §12).</summary>
    HeldOffline = 10,
}

/// <summary>What evidence backs the current claim. Never inflated.</summary>
public enum ConfidenceLevel
{
    /// <summary>The link took the bytes. Says nothing about the printer.</summary>
    TransportAccepted = 0,

    /// <summary>The printer reported itself healthy.</summary>
    PrinterHealthy = 1,

    /// <summary>An ordered fence for the print itself came back.</summary>
    PrintConfirmed = 2,

    /// <summary>An ordered fence after the cut came back.</summary>
    CutProcessed = 3,

    /// <summary>The post-cut cutter read came back clean.</summary>
    CutFaultFree = 4,

    /// <summary>Verified outside the SDK entirely.</summary>
    PhysicallyVerified = 5,
}

/// <summary>The per-printer stream that replaces availability polling.</summary>
public enum DeviceEvent
{
    /// <summary>The printer is online.</summary>
    Online = 0,

    /// <summary>The printer is offline.</summary>
    Offline = 1,

    /// <summary>The cover was opened.</summary>
    CoverOpen = 2,

    /// <summary>The cover was closed.</summary>
    CoverClosed = 3,

    /// <summary>Paper ran out.</summary>
    PaperOut = 4,

    /// <summary>Paper is near the end of the roll.</summary>
    PaperNearEnd = 5,

    /// <summary>Paper is present and fine.</summary>
    PaperOk = 6,

    /// <summary>The autocutter reported a fault.</summary>
    CutterError = 7,

    /// <summary>A recoverable error was reported.</summary>
    RecoverableError = 8,

    /// <summary>An unrecoverable error was reported.</summary>
    UnrecoverableError = 9,

    /// <summary>The link dropped.</summary>
    ConnectionLost = 10,

    /// <summary>The link came back.</summary>
    ConnectionRestored = 11,

    /// <summary>A GS(H) echo arrived carrying a token this driver never issued —
    /// something else is writing to the same printer (docs/api.md §14).</summary>
    ForeignWriterDetected = 12,
}

/// <summary>
/// What class of evidence backs a result — docs/compatibility-brief.md §24.
/// </summary>
/// <remarks>
/// Orthogonal to <see cref="ConfidenceLevel"/>: the level says how far up the evidence
/// ladder a job climbed, the grade says what the claim is made of. Done at
/// <see cref="ConfidenceLevel.CutProcessed"/> on grade A and Done at
/// <see cref="ConfidenceLevel.CutProcessed"/> on grade D are not the same claim. The
/// members are ordered strongest-first, so a caller may compare them numerically.
/// </remarks>
public enum ConfidenceGrade
{
    /// <summary>
    /// A durable, queryable printer-side job: ePOS submits, returns a JobID, and the
    /// result is retrievable afterwards — the only mechanism in the hierarchy that
    /// survives the application losing its connection between submission and answer.
    /// <para>
    /// Nothing produces this grade yet. The ePOS transport does not exist in this core, so
    /// no <see cref="JobResult"/> can carry it. It is declared now because the ABI's enums
    /// are closed and mirrored by four wrappers, and adding a member later would renumber
    /// every mirror a second time. A profile that reports an ePOS JobID is describing
    /// hardware, not making a claim, and is graded <see cref="AJobLevelConfirmation"/>.
    /// </para>
    /// </summary>
    APlusDurableQueryableJob = 0,

    /// <summary>Job-level confirmation: GS(H) fn48, a Star checked block.</summary>
    AJobLevelConfirmation = 1,

    /// <summary>Ordered device response with weaker semantics (GS r, a vendor idle query).</summary>
    BOrderedDeviceResponse = 2,

    /// <summary>Device status around transmission (DLE EOT, ASB, SNMP).</summary>
    CDeviceStatusAround = 3,

    /// <summary>A spooler or IPP gateway said completed.</summary>
    DSpoolerCompleted = 4,

    /// <summary>Transport only: a write succeeded somewhere.</summary>
    ETransportOnly = 5,
}

/// <summary>
/// Where the claim that a printer has a capability comes from —
/// docs/compatibility-brief.md §28.
/// </summary>
/// <remarks>
/// Recognising ESC/POS <em>print</em> commands does not prove the Epson <em>feedback</em>
/// extensions, so "the manufacturer's manual says so", "we asked the hardware and it
/// answered" and "nobody has checked" are three answers rather than one boolean. The
/// three are independent rather than ordered: a probe can contradict documentation when
/// the interface path swallows responses, and documentation can cover a model no probe
/// has reached.
/// </remarks>
public enum Provenance
{
    /// <summary>The manufacturer's own command documentation lists it for this model.</summary>
    Documented = 0,

    /// <summary>
    /// This driver asked the installed hardware over the installed interface path and it
    /// answered. Specific to the path it was measured on, and stronger than marketing.
    /// </summary>
    Probed = 1,

    /// <summary>
    /// Neither — a shipped default nobody has confirmed, which is what "ESC/POS
    /// compatible" on a datasheet amounts to.
    /// </summary>
    Unverified = 2,
}

/// <summary>
/// What a device actually speaks — docs/compatibility-brief.md §1.
/// </summary>
/// <remarks>
/// Only <see cref="EscPos"/> is implemented. The rest exist so a fleet containing them can
/// be described rather than misdriven: a profile naming ZPL, CPCL, Brother raster or ESC/P
/// is refused with <see cref="FailureReason.Unsupported"/> before a byte is written,
/// because an ESC/POS engine pointed at a Zebra prints a metre of text instead of a label.
/// </remarks>
public enum CommandLanguage
{
    /// <summary>Epson ESC/POS, the only language this core drives.</summary>
    EscPos = 0,

    /// <summary>Star's native command set.</summary>
    StarPrnt = 1,

    /// <summary>Star line mode.</summary>
    StarLine = 2,

    /// <summary>Epson ePOS XML, spoken by the intelligent printers over their own transport.</summary>
    EposXml = 3,

    /// <summary>Zebra Programming Language (Link-OS).</summary>
    Zpl = 4,

    /// <summary>Comtec/Zebra mobile, also documented by the Citizen CMP portables.</summary>
    Cpcl = 5,

    /// <summary>Brother's raster command reference.</summary>
    BrotherRaster = 6,

    /// <summary>Brother ESC/P — a different language from Epson ESC/POS, despite the name.</summary>
    EscP = 7,
}

/// <summary>Who is authoritative for the completion claim.</summary>
public enum CompletionAuthority
{
    /// <summary>The physical printer's own mechanism answered.</summary>
    PhysicalPrinter = 0,

    /// <summary>A vendor spooler answered (e.g. ePOS).</summary>
    VendorSpooler = 1,

    /// <summary>The pd agent daemon answered.</summary>
    PdAgent = 2,

    /// <summary>An intermediate print server answered.</summary>
    PrintServer = 3,

    /// <summary>Nothing beyond the transport accepted the bytes.</summary>
    TransportOnly = 4,
}

/// <summary>Why a job failed, or why its outcome is unknown.</summary>
public enum FailureReason
{
    /// <summary>No failure.</summary>
    None = 0,

    /// <summary>The printer could not be reached at all.</summary>
    TransportUnreachable = 1,

    /// <summary>Preflight found the cover open.</summary>
    PreflightCoverOpen = 2,

    /// <summary>Preflight found no paper.</summary>
    PreflightPaperOut = 3,

    /// <summary>Preflight found a hardware error.</summary>
    PreflightHardwareError = 4,

    /// <summary>The completion fence never came back in time.</summary>
    TimeoutAwaitingCompletion = 5,

    /// <summary>The autocutter faulted.</summary>
    CutterFault = 6,

    /// <summary>The printer cannot do what was asked.</summary>
    Unsupported = 7,

    /// <summary>Nothing more specific can be said.</summary>
    Unknown = 8,

    /// <summary>Print-queue addon only: the job outlived its deadline.</summary>
    Expired = 9,

    /// <summary>Print-queue addon only: the queue was full.</summary>
    QueueOverflow = 10,
}

/// <summary>
/// The terminal answer as a raw value. Applications should use <see cref="JobResult"/>,
/// which makes the three cases impossible to collapse into a boolean; this exists because
/// the enum bridge has to be able to compare member-for-member with the core.
/// </summary>
public enum JobOutcome
{
    /// <summary>It printed.</summary>
    Done = 0,

    /// <summary>It did not print, and that is known.</summary>
    Failed = 1,

    /// <summary>It may or may not have printed.</summary>
    Unknown = 2,
}

/// <summary>What to do with the paper at the end of a job.</summary>
public enum CutSetting
{
    /// <summary>Whatever this printer's cutter does.</summary>
    Profile = 0,

    /// <summary>Partial cut.</summary>
    Partial = 1,

    /// <summary>Full cut.</summary>
    Full = 2,

    /// <summary>Do not cut.</summary>
    None = 3,
}

/// <summary>Whether to check the device before sending a payload.</summary>
public enum PreflightMode
{
    /// <summary>Refuse the job when the device is not fit to print.</summary>
    Strict = 0,

    /// <summary>Send regardless.</summary>
    Skip = 1,
}

/// <summary>The tier a job was submitted as (docs/api.md §3).</summary>
public enum PayloadKind
{
    /// <summary>Straight RGBA8 pixels; the core does the imaging.</summary>
    Raster = 0,

    /// <summary>A short list of receipt operations.</summary>
    Document = 1,

    /// <summary>Bytes passed through verbatim.</summary>
    Raw = 2,
}

/// <summary>Which ordered fence a profile answers.</summary>
public enum CompletionMechanism
{
    /// <summary>GS ( H process-ID response.</summary>
    GsParenH = 0,

    /// <summary>Queued GS r 1 status.</summary>
    GsR1 = 1,

    /// <summary>A vendor idle report.</summary>
    VendorIdle = 2,

    /// <summary>An ePOS job identifier.</summary>
    EposJobId = 3,

    /// <summary>Star's checked block mode.</summary>
    StarCheckedBlock = 4,

    /// <summary>No completion fence at all: the job can never do better than Unknown.</summary>
    None = 5,

    /// <summary>
    /// Star's ETB fence: 0x17 plus the five-bit ASB print-end counter (M13b,
    /// docs/wire-protocols.md section 2). Selected only where the driver holds the
    /// printer's session exclusively, because the ASB frame carrying that counter is
    /// broadcast to every host connected to TCP 9100 and cannot say whose data finished.
    /// </summary>
    StarEtb = 6,

    /// <summary>
    /// Star's ESC GS ETX fence, which echoes the correlation bytes it was handed and
    /// replies only to the issuing session. The default on every Star model the core
    /// drives itself.
    /// </summary>
    StarEscGsEtx = 7,
}

/// <summary>The cut a profile's mechanism actually performs.</summary>
public enum CutVariant
{
    /// <summary>Partial cut.</summary>
    Partial = 0,

    /// <summary>Full cut.</summary>
    Full = 1,

    /// <summary>No cutter.</summary>
    None = 2,
}

/// <summary>Text alignment; values are the <c>n</c> operand of <c>ESC a n</c>.</summary>
public enum Alignment
{
    /// <summary>Left aligned.</summary>
    Left = 0,

    /// <summary>Centred.</summary>
    Center = 1,

    /// <summary>Right aligned.</summary>
    Right = 2,
}

/// <summary>
/// Code pages the document tier supports. The values are the <c>n</c> operand of
/// <c>ESC t n</c>, so they are not contiguous — there are five members, not twenty.
/// </summary>
public enum CodePage
{
    /// <summary>PC437, the US/European default.</summary>
    PC437 = 0,

    /// <summary>PC850, multilingual Latin 1.</summary>
    PC850 = 2,

    /// <summary>Windows-1252.</summary>
    WPC1252 = 16,

    /// <summary>PC852, Latin 2.</summary>
    PC852 = 18,

    /// <summary>PC858, Latin 1 with the euro sign.</summary>
    PC858 = 19,
}

/// <summary>How the raster tier turns grey into dots.</summary>
public enum Binarization
{
    /// <summary>One threshold for every pixel.</summary>
    FixedThreshold = 0,

    /// <summary>Floyd-Steinberg error diffusion.</summary>
    FloydSteinberg = 1,
}

// --- M14: cash drawer (docs/cash-drawer.md) -------------------------------------------
//
// The drawer is a separate printer peripheral: its own electrical profile, its own
// command method, its own feedback method, none of them derivable from the others. The
// four enums below are the closed sets that describe it.

/// <summary>
/// What is known about a cash drawer.
/// </summary>
/// <remarks>
/// Deliberately not a boolean. Sending <c>ESC p</c> and watching the microswitch change
/// are different claims, and an operator at a till needs them apart:
/// <see cref="DrawerState.KickSentUnverified"/> is a real answer rather than a softer
/// success. Through a cheap print server the pulse travels forward while the sensor
/// response never comes back, and that path can only ever produce it.
/// </remarks>
public enum DrawerState
{
    /// <summary>The switch answered and, with a calibrated polarity, says the drawer is shut.</summary>
    Closed = 0,

    /// <summary>Already open. Reading this before a pulse is what stops a redundant kick.</summary>
    Open = 1,

    /// <summary>A pulse is on the wire and the verification window has not closed.</summary>
    Opening = 2,

    /// <summary>The pulse was accepted by the link and nothing can confirm what happened next.</summary>
    KickSentUnverified = 3,

    /// <summary>The switch was seen changing. The only member that claims physical movement.</summary>
    OpenVerified = 4,

    /// <summary>The pulse went out and the switch never moved: locked, jammed, wrong channel, wrong cable.</summary>
    FailedToOpen = 5,

    /// <summary>This port has no switch input wired or documented. Not an error.</summary>
    NoSensor = 6,

    /// <summary>Nothing is known — unclassified port, unsupported method, or an uncalibrated polarity.</summary>
    Unknown = 7,
}

/// <summary>
/// The electrical classification of a drawer port.
/// </summary>
/// <remarks>
/// RJ11/RJ12-looking drawer connectors are not a universal electrical standard. Star's
/// identical-looking 6P6C socket carries +24 V on pin 3 and the sense line on pin 6,
/// precisely where Epson puts sense and signal ground — which is why printer-specific
/// drawer cables exist for plugs that look the same.
/// </remarks>
public enum DrawerPortStandard
{
    /// <summary>1 FG, 2 kick 1, 3 sensor, 4 +24 V, 5 kick 2, 6 signal ground.</summary>
    Epson24V6P6C = 0,

    /// <summary>The same plug with +24 V on pin 3 and the sense line on pin 6.</summary>
    Star24V6P6C = 1,

    /// <summary>The 12 V exceptions, common on 58 mm hardware.</summary>
    Generic12V6P6C = 2,

    /// <summary>Unclassified. Nothing is ever energised on one of these.</summary>
    Unknown = 3,
}

/// <summary>
/// Which software path fires the drawer. Independent of the cable pinout: two printers
/// accepting the same "kick drawer 1" command may still wire the socket differently.
/// </summary>
public enum DrawerKickMethod
{
    /// <summary><c>ESC p m t1 t2</c>, queued behind print data.</summary>
    EpsonEscP = 0,

    /// <summary>The ePOS peripheral API — never raw bytes smuggled into print XML.</summary>
    EpsonEpos = 1,

    /// <summary>StarPRNT <c>appendPeripheral(...)</c>.</summary>
    StarPrnt = 2,

    /// <summary>The Bixolon SDK's <c>makeDKout</c>.</summary>
    BixolonSdk = 3,

    /// <summary><c>ESC p</c>, with the "cannot fire while printing" serialisation quirk.</summary>
    CitizenEscP = 4,

    /// <summary><c>ESC p</c>; the realtime <c>DLE DC4</c> variant exists and is not preferred.</summary>
    SnbcEscP = 5,

    /// <summary>Documented, vendor-specific, not implemented here.</summary>
    Vendor = 6,

    /// <summary>No drawer path on this device, or none this engine may use.</summary>
    Unsupported = 7,
}

/// <summary>How the drawer switch is read back.</summary>
public enum DrawerStatusMethod
{
    /// <summary><c>GS r 2</c> / <c>GS r 50</c> — queued drawer-kick-out connector status.</summary>
    GsR2 = 0,

    /// <summary>The drawer bit inside an automatic status back frame.</summary>
    Asb = 1,

    /// <summary>Star's <c>drawerOpenCloseSignal</c>.</summary>
    StarSignal = 2,

    /// <summary>The vendor SDK's own drawer status call.</summary>
    VendorSdk = 3,

    /// <summary>No readable switch.</summary>
    None = 4,
}

/// <summary>
/// The core's own spelling of every mirrored enum member, straight from the C ABI.
/// </summary>
/// <remarks>
/// <para>
/// These are not <c>ToString()</c>. A .NET name is this wrapper's spelling of a member;
/// an ABI name is the core's, which is what the journal records, what <c>pdctl</c> prints
/// and what a support engineer reads in a log six months later. Rendering a diagnostic
/// with the wrapper's spelling and then searching the journal for it is a wasted
/// afternoon, so the core's spelling is available everywhere the value is.
/// </para>
/// <para>
/// Every string comes from static storage inside the library and is never null.
/// </para>
/// </remarks>
public static class AbiNames
{
    /// <summary><c>pd_job_state_name</c>.</summary>
    /// <param name="value">The state.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this JobState value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_job_state_name((int)value));

    /// <summary><c>pd_confidence_level_name</c>.</summary>
    /// <param name="value">The level.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this ConfidenceLevel value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_confidence_level_name((int)value));

    /// <summary><c>pd_device_event_name</c>.</summary>
    /// <param name="value">The event.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this DeviceEvent value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_device_event_name((int)value));

    /// <summary><c>pd_failure_reason_name</c>.</summary>
    /// <param name="value">The reason.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this FailureReason value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_failure_reason_name((int)value));

    /// <summary><c>pd_job_outcome_name</c>.</summary>
    /// <param name="value">The outcome.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this JobOutcome value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_job_outcome_name((int)value));

    /// <summary><c>pd_confidence_grade_name</c>.</summary>
    /// <param name="value">The grade.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this ConfidenceGrade value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_confidence_grade_name((int)value));

    /// <summary>
    /// <c>pd_confidence_grade_letter</c> — "A+", "A".."E", the letter a report tabulates
    /// where the member name is too long.
    /// </summary>
    /// <param name="value">The grade.</param>
    /// <returns>The letter.</returns>
    public static string Letter(this ConfidenceGrade value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_confidence_grade_letter((int)value));

    /// <summary><c>pd_completion_authority_name</c>.</summary>
    /// <param name="value">The authority.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this CompletionAuthority value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_completion_authority_name((int)value));

    /// <summary><c>pd_provenance_name</c>.</summary>
    /// <param name="value">The provenance.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this Provenance value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_provenance_name((int)value));

    /// <summary><c>pd_command_language_name</c>.</summary>
    /// <param name="value">The language.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this CommandLanguage value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_command_language_name((int)value));

    /// <summary><c>pd_payload_kind_name</c>.</summary>
    /// <param name="value">The tier.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this PayloadKind value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_payload_kind_name((int)value));

    /// <summary><c>pd_completion_mechanism_name</c>.</summary>
    /// <param name="value">The mechanism.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this CompletionMechanism value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_completion_mechanism_name((int)value));

    /// <summary><c>pd_cut_variant_name</c>.</summary>
    /// <param name="value">The variant.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this CutVariant value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_cut_variant_name((int)value));

    /// <summary><c>pd_drawer_state_name</c>.</summary>
    /// <param name="value">The state.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this DrawerState value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_drawer_state_name((int)value));

    /// <summary><c>pd_drawer_port_standard_name</c>.</summary>
    /// <param name="value">The port standard.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this DrawerPortStandard value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_drawer_port_standard_name((int)value));

    /// <summary><c>pd_drawer_kick_method_name</c>.</summary>
    /// <param name="value">The kick method.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this DrawerKickMethod value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_drawer_kick_method_name((int)value));

    /// <summary><c>pd_drawer_status_method_name</c>.</summary>
    /// <param name="value">The status method.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this DrawerStatusMethod value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_drawer_status_method_name((int)value));

    /// <summary><c>pd_profile_selection_name</c>.</summary>
    /// <param name="value">How the profile was arrived at.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this ProfileSelection value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_profile_selection_name((int)value));

    /// <summary><c>pd_detection_status_name</c>.</summary>
    /// <param name="value">What detection established.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this DetectionStatus value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_detection_status_name((int)value));

    /// <summary><c>pd_drain_order_name</c>.</summary>
    /// <param name="value">The drain order.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this DrainOrder value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_drain_order_name((int)value));

    /// <summary><c>pd_match_kind_name</c>.</summary>
    /// <param name="value">A custom matcher's verdict.</param>
    /// <returns>The core's spelling.</returns>
    public static string AbiName(this CompletionMatchKind value) =>
        NativeMethods.ReadUtf8(NativeMethods.pd_match_kind_name((int)value));
}
