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
