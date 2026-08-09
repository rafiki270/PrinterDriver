using System.Runtime.InteropServices;

namespace PrinterDriver;

// P/Invoke declarations for capi/include/printerdriver/pd.h -- the C ABI docs/api.md §9
// calls "the whole surface". This file binds that header and nothing else: never the C++
// headers under core/include, never capi/src/pd_internal.hpp. pd.h's own header comment
// says wrappers are "generated-thin ... enum bridging plus async adapters, no logic", and
// binding the C++ side directly would break the guarantee the static_asserts in
// pd_capi.cpp exist to enforce.
//
// -- Struct layouts -------------------------------------------------------------------
// Every struct below is blittable and laid out to match the C one on a 64-bit target.
// The layouts were derived from pd.h field by field; PrinterDriver.Tests/AbiLayoutTests.cs
// asserts the sizes against the values the C compiler produces, so a field reordered in
// pd.h fails a test rather than silently reading the wrong bytes.
//
// -- Callback lifetime ----------------------------------------------------------------
// pd.h has no pd_unsubscribe_*. A delegate handed to pd_subscribe_job or
// pd_subscribe_device must therefore stay alive and un-relocated until pd_destroy has
// returned. See CallbackRoots for how that is enforced; getting it wrong produces a
// collected-delegate crash on a printer worker thread, which is the single most common
// bug in a P/Invoke wrapper of a callback API.

internal static class NativeMethods
{
    /// <summary>Name passed to the loader; NativeLibraryResolver maps it to a file.</summary>
    internal const string LibraryName = "printerdriver_capi";

    // --- Callback signatures ---------------------------------------------------------

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void JobEventCallback(nint job, PdJobEvent jobEvent, nint context);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DeviceEventCallback(nint printer, int deviceEvent, nint context);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void LogCallback(nint message, nint context);

    // The three operations a custom transport implements (docs/compatibility-brief.md
    // §25). Unlike the subscription callbacks these are not invoked "eventually": the
    // core's worker thread blocks inside them while a job is being sent, one at a time
    // and never concurrently with each other. `data` points into the core's own send
    // buffer and is valid only for the duration of the call.

    /// <summary>Non-zero for success. Returning 0 fails the job with a transport reason.</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate int TransportConnectCallback(nint context);

    /// <summary>
    /// Bytes actually transferred, or negative for a hard failure. A short write must be
    /// reported honestly rather than rounded up: zero bytes out is a known failure and one
    /// byte out is Unknown (docs/api.md §4), and that is what decides a reprint.
    /// </summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate long TransportWriteCallback(nint context, nint data, nuint size);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void TransportCloseCallback(nint context);

    // --- Driver ----------------------------------------------------------------------

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_create(in PdConfig config);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void pd_destroy(nint driver);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_last_error(nint driver);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_profile_ids();

    // --- Printers --------------------------------------------------------------------

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_add_printer_tcp(nint driver, in PdTcpConfig config);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_add_printer_custom(nint driver, in PdTransportVtable vtable,
                                                      nint context, nint profileId,
                                                      uint widthDots);

    /// <summary>
    /// Delivers bytes the link received. Safe from any thread — including while a write is
    /// in flight, which is the normal case — but never from inside connect/write/close:
    /// those run on the thread that would have to service the delivery. Returns 0 when
    /// nothing was listening, which is information rather than an error.
    /// </summary>
    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_transport_feed_bytes(nint printer, nint data, nuint size);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_transport_link_dropped(
        nint printer, [MarshalAs(UnmanagedType.LPUTF8Str)] string message);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_printer_id(nint printer);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint pd_printer_width_dots(nint printer);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_printer_completion(nint printer);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_printer_completion_provenance(nint printer);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_printer_language(nint printer);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern PdDeviceStatus pd_printer_status(nint driver, nint printer);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern PdDeviceStatus pd_printer_refresh_status(nint driver, nint printer,
                                                                    uint timeoutMs);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void pd_open_cash_drawer(nint driver, nint printer);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void pd_printer_drain(nint driver, nint printer);

    // --- M14: cash drawer (docs/cash-drawer.md) --------------------------------------

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern PdDrawerCapabilities pd_printer_drawer_capabilities(nint printer);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern PdDrawerResult pd_drawer_open(nint driver, nint printer,
                                                          in PdDrawerRequest request);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern PdDrawerReading pd_drawer_read_sensor(nint driver, nint printer,
                                                                 uint timeoutMs);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_drawer_calibrate_polarity(nint driver, nint printer,
                                                             int highMeansOpen);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_drawer_polarity_calibrated(nint driver, nint printer);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_drawer_high_means_open(nint driver, nint printer);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_drawer_state_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_drawer_port_standard_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_drawer_kick_method_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_drawer_status_method_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void pd_subscribe_device(nint driver, nint printer,
                                                    DeviceEventCallback callback, nint context);

    // --- M13b: the print-queue addon (docs/sdk-spec.md §12) ---------------------------

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_queue_create(nint driver, in PdQueuePolicy policy);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void pd_queue_destroy(nint queue);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_queue_enqueue(nint queue, nint printer,
                                                 in PdPayload payload,
                                                 in PdQueueOptions options);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void pd_queue_pause(nint queue,
                                               [MarshalAs(UnmanagedType.LPUTF8Str)] string printerId);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void pd_queue_resume(nint queue,
                                                [MarshalAs(UnmanagedType.LPUTF8Str)] string printerId);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_queue_is_paused(nint queue,
                                                  [MarshalAs(UnmanagedType.LPUTF8Str)] string printerId);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_queue_is_blocked(nint queue,
                                                   [MarshalAs(UnmanagedType.LPUTF8Str)] string printerId);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void pd_queue_unblock(nint queue,
                                                 [MarshalAs(UnmanagedType.LPUTF8Str)] string printerId);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nuint pd_queue_pending(nint queue,
                                                  [MarshalAs(UnmanagedType.LPUTF8Str)] string? printerId);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nuint pd_queue_expired_count(nint queue);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nuint pd_queue_overflow_count(nint queue);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nuint pd_queue_drained_count(nint queue);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void pd_queue_tick(nint queue);

    // --- Jobs ------------------------------------------------------------------------

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_print(nint driver, nint printer, in PdPayload payload,
                                         in PdJobOptions options);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_force_reprint(nint driver, nint printer,
                                                 [MarshalAs(UnmanagedType.LPUTF8Str)] string key,
                                                 in PdJobOptions options);

    /// <summary>
    /// <c>pd_force_reprint</c> with control over the banner. pd.h defines
    /// <c>pd_force_reprint</c> as exactly this call with an all-zeroes
    /// <c>pd_reprint_options</c>, i.e. with the banner on.
    /// </summary>
    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_force_reprint_opts(nint driver, nint printer,
                                                      [MarshalAs(UnmanagedType.LPUTF8Str)] string key,
                                                      in PdReprintOptions options);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_find_job(nint driver,
                                            [MarshalAs(UnmanagedType.LPUTF8Str)] string key);

    /// <summary>
    /// Paper to job (docs/api.md §14): resolves either of a job's four-character
    /// verification identifiers, most-recent-first, including jobs reloaded from the
    /// journal.
    /// </summary>
    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_job_by_token(nint driver,
                                                [MarshalAs(UnmanagedType.LPUTF8Str)] string token);

    /// <summary>
    /// The two characters every token this driver issues starts with. Persisted, so it
    /// survives a restart.
    /// </summary>
    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_instance_nonce(nint driver);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_job_id(nint job);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_job_key(nint job);

    /// <summary>
    /// The identifier the ticket printed as <c>V:</c>. Empty on a printer whose completion
    /// mechanism is not <c>GS ( H</c> — there is no wire token to promote — and until the
    /// job reaches a worker.
    /// </summary>
    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_job_print_token(nint job);

    /// <summary>The identifier the job's cut fence carried. Same rules.</summary>
    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_job_cut_token(nint job);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern uint pd_job_attempt(nint job);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_job_current_state(nint job);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_job_confidence(nint job);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_job_is_terminal(nint job);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void pd_subscribe_job(nint driver, nint job, JobEventCallback callback,
                                                 nint context);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_job_await(nint driver, nint job, uint timeoutMs,
                                            out PdJobResult result);

    // --- Enum names -------------------------------------------------------------------

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_job_state_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_confidence_level_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_device_event_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_failure_reason_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_job_outcome_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_confidence_grade_name(int value);

    /// <summary>"A+", "A".."E" — the letter a report tabulates.</summary>
    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_confidence_grade_letter(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_completion_authority_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_provenance_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_command_language_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_payload_kind_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_completion_mechanism_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_cut_variant_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_drain_order_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_match_kind_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_code_page_at(int index);

    // --- M15: self-test, auto-detection and LAN discovery (docs/api.md §15) ----------

    /// <summary>
    /// One classified candidate, fired from a sweep worker thread. The struct and every
    /// string in it are valid only for the duration of the call.
    /// </summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DetectedCallback(nint printer, ulong completed, ulong total,
        nint context);

    /// <summary>One open port, fired from a sweep worker thread. Same lifetime rule.</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void DiscoveredCallback(nint device, ulong completed, ulong total,
        nint context);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_self_test(nint driver, nint printer,
        in PdSelfTestOptions options, out PdSelfTestResult result);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_auto_detect(nint driver, in PdAutoDetectOptions options,
        DetectedCallback? callback, nint context);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_discover(nint driver, in PdDiscoverOptions options,
        DiscoveredCallback? callback, nint context);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_local_subnet(nint driver);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_profile_selection_name(int value);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_detection_status_name(int value);

    // --- M16: custom method registration (docs/api.md §16) ----------------------------
    //
    // Five extension points, all per-driver, all data-plus-callbacks. Every callback here
    // is invoked on a CORE thread and must answer there and then -- a fence's bytes, a
    // matcher's verdict, a formatter's text. .NET can serve that: a rooted delegate is
    // callable from any thread and returns a value, which is why these are managed
    // callbacks and not (as in the Dart wrapper) native function pointers the application
    // has to supply. The rooting is not optional; see CallbackRoots.

    /// <summary>
    /// Writes the fence bytes for a job's four-character token into <c>output</c> and
    /// returns the count. A fence longer than <c>capacity</c> must return more than the
    /// capacity rather than truncate: the core then fails that job Unknown instead of
    /// sending half a fence.
    /// </summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate nuint FenceBytesCallback(nint context, nint jobToken, nint output,
                                               nuint capacity);

    /// <summary>Classifies the printer→host bytes accumulated since the last verdict.</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate PdMatchResult CompletionMatcherCallback(nint context, nint data,
                                                              nuint size);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate PdProbeFinding ProbeClassifyCallback(nint context, nint response,
                                                            nuint size);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate nuint BlockHandlerCallback(nint context, nint blockJson, nint profileJson,
                                                  nint output, nuint capacity, nint ok,
                                                  nint detail, nuint detailCapacity);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate nuint FormatterCallback(nint context, nint value, nint args, nint locale,
                                               nint output, nuint capacity, nint handled);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate nuint DrawerKickBytesCallback(nint context, byte channel, ushort pulseMs,
                                                     nint output, nuint capacity);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate nuint DrawerStatusRequestCallback(nint context, nint output,
                                                         nuint capacity);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate int DrawerStatusParseCallback(nint context, nint response, nuint size);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_register_completion_method(nint driver,
                                                             in PdCompletionMethod method);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_register_probe_step(nint driver, in PdProbeStep step);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_register_block_handler(nint driver,
                                                         in PdBlockHandler handler);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_register_formatter(nint driver, in PdFormatter formatter);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int pd_register_drawer_kick(nint driver, in PdDrawerKickReg reg);

    // --- Helpers ---------------------------------------------------------------------

    /// <summary>
    /// A <c>const char*</c> the ABI owns. pd.h: nothing returned by it is ever freed by
    /// the caller, so this copies rather than taking ownership.
    /// </summary>
    internal static string ReadUtf8(nint pointer) =>
        pointer == 0 ? string.Empty : Marshal.PtrToStringUTF8(pointer) ?? string.Empty;
}

// --- Structs -------------------------------------------------------------------------

[StructLayout(LayoutKind.Sequential)]
internal struct PdConfig
{
    public nint StorageDirectory;
    public int FsyncDisabled;
    public nint Log;
    public nint LogContext;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdTcpConfig
{
    public nint PrinterId;
    public nint Host;
    public ushort Port;
    public uint WidthDots;
    public nint ProfileId;
    public uint ConnectTimeoutMs;
}

/// <summary>
/// <c>pd_transport_vtable</c>: the three operations a caller-owned link implements, plus
/// the string its printer id derives from.
/// </summary>
/// <remarks>
/// The function pointers are held as raw <see cref="nint"/> rather than as delegate-typed
/// fields, which keeps the struct blittable and makes the delegate lifetime explicit at
/// the call site: <see cref="Marshal.GetFunctionPointerForDelegate{TDelegate}"/> hands out
/// a stub that lives exactly as long as the delegate it came from, and the ABI keeps
/// calling through it until <c>pd_destroy</c>. Rooting is therefore not optional — see
/// CallbackRoots.
/// </remarks>
[StructLayout(LayoutKind.Sequential)]
internal struct PdTransportVtable
{
    public nint Connect;
    public nint Write;
    public nint Close;
    public nint Description;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdJobOptions
{
    public nint Key;
    public int Cut;
    public int OpenDrawer;
    public int Preflight;
    public uint TimeoutMs;
    public uint TopFeedDots;
    public uint BottomFeedDots;
    public int SuppressVerificationId;
}

/// <summary>
/// <c>pd_queue_policy</c>. All-zeroes is a pure serializer: hold nothing, never expire,
/// unlimited depth, FIFO.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PdQueuePolicy
{
    public int HoldWhileOffline;
    public uint DefaultTtlMs;
    public uint MaxDepth;
    public int DrainOrder;
}

/// <summary><c>pd_queue_options</c>.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PdQueueOptions
{
    public nint Key;
    public uint TtlMs;
    public int Priority;
    public int Cut;
    public int OpenDrawer;
    public int Preflight;
    public uint TimeoutMs;
}

// --- M14: cash drawer (docs/cash-drawer.md) -------------------------------------------

[StructLayout(LayoutKind.Sequential)]
internal struct PdDrawerCapabilities
{
    public int Present;
    public int Standard;
    public ushort Voltage;
    public ushort MaxCurrentMa;
    public byte ChannelCount;
    public byte SensorPin;
    public int Method;
    public ushort DefaultPulseMs;
    public ushort MaxPulseMs;
    public ushort CooldownMs;
    public int CanKickDuringPrint;
    public int StatusAvailable;
    public int StatusMethod;
    public int SharedBetweenDrawers;
    public int SharedWithBuzzer;
    public int ElectricalProvenance;
    public int CommandsProvenance;
    public int Kickable;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdDrawerRequest
{
    public byte Channel;
    public ushort PulseMs;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdDrawerResult
{
    public int State;
    public int PreviousState;
    public byte Channel;
    public ushort PulseMs;
    public uint ElapsedMs;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdDrawerReading
{
    public int Available;
    public int Answered;
    public int PinHigh;
    public int NeedsCalibration;
    public int State;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdJobEvent
{
    public int State;
    public int Confidence;
    public int HasReason;
    public int Reason;
    public ulong MonotonicMs;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdJobResult
{
    public int Outcome;
    public int Confidence;
    public int Reason;
    public int Grade;
    public int Authority;
    public nint Method;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdDeviceStatus
{
    public int Connected;
    public int Observed;
    public int Online;
    public int CoverOpen;
    public int PaperOut;
    public int PaperNearEnd;
    public int CutterError;
    public int UnrecoverableError;
    public int RecoverableError;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdRasterRgba8
{
    public nint Pixels;
    public uint Width;
    public uint Height;
    public uint StrideBytes;
    public int Binarization;
    public byte Threshold;
    public uint MaxRowsPerBand;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdOp
{
    public int Kind;
    public nint Text;
    public int Value;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdDocument
{
    public nint Ops;
    public nuint Count;
    public int CodePage;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdRaw
{
    public nint Bytes;
    public nuint Size;
}

/// <summary>
/// The <c>union</c> inside <c>pd_payload</c>. Overlapping is safe because every member is
/// blittable: there is not a single GC reference in here, only raw pointers the caller
/// keeps pinned for the duration of the pd_print call.
/// </summary>
[StructLayout(LayoutKind.Explicit)]
internal struct PdPayloadUnion
{
    [FieldOffset(0)] public PdRasterRgba8 Raster;
    [FieldOffset(0)] public PdDocument Document;
    [FieldOffset(0)] public PdRaw Raw;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdPayload
{
    public int Kind;
    public PdPayloadUnion As;
}

// --- M15 structs (docs/api.md §15) ----------------------------------------------------

/// <summary>
/// <c>pd_detection_summary</c> — the report the self-test prints on paper and
/// auto-detection returns per candidate. Every <c>nint</c> here is a
/// <c>const char*</c> the driver owns; see pd.h for how long.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PdDetectionSummary
{
    public nint Endpoint;
    public nint Vendor;
    public nint Model;
    public nint Firmware;
    public nint Serial;
    public int IdentityTrusted;
    public byte ConfidencePercent;
    public int ImpersonationSuspected;
    public int IdentityFresh;
    public nint ProfileId;
    public int Selection;
    public ushort NominalPaperMm;
    public uint PrintableWidthDots;
    public uint CharsPerLine;
    public ushort Dpi;
    public int Completion;
    public int GradeCeiling;
    public int Authority;
    public nint Method;
    public int CompletionProvenance;
    public int DrawerPresent;
    public int DrawerKickable;
    public int DrawerStandard;
    public ushort DrawerVoltage;
    public int DrawerElectricalProvenance;
    public int DrawerCommandsProvenance;
    public nint ProvenanceSummary;
    public nint Degradations;
    public nuint DegradationCount;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdSelfTestOptions
{
    public nint Key;
    public int RefreshIdentity;
    public int ProbeWithoutPrinting;
    public int NoBarcode;
    public nint BarcodeData;
    public int NoVerificationId;
    public uint TimeoutMs;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdSelfTestResult
{
    public PdJobResult Result;
    public PdDetectionSummary Detection;
    public nint Key;
    public nint PrintToken;
    public nint TicketText;
    public nint Job;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdAutoDetectOptions
{
    public nint SubnetCidr;
    public nint Endpoints;
    public ushort Port;
    public uint Concurrency;
    public uint ConnectTimeoutMs;
    public uint ResponseTimeoutMs;
    public int LeaveUnknownUnprobed;
    public uint StatusTimeoutMs;
    public uint IdentityTimeoutMs;
    public uint CompletionTimeoutMs;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdDetectedPrinter
{
    public nint Endpoint;
    public nint Host;
    public ushort Port;
    public int Status;
    public int PortOpen;
    public int FromCache;
    public nint DleEotHex;
    public PdDetectionSummary Summary;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdDiscoverOptions
{
    public nint SubnetCidr;
    public ushort Port;
    public uint Concurrency;
    public uint ConnectTimeoutMs;
    public uint ResponseTimeoutMs;
    public int NoBackchannelProbe;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdDiscoveredDevice
{
    public nint Ip;
    public ushort Port;
    public int Port9100Open;
    public nint DleEotHex;
}

/// <summary>
/// <c>pd_reprint_options</c>. All-zeroes means the banner prints, which is why
/// <c>SuppressBanner</c> is spelled as the negative: a duplicate receipt that does not
/// announce itself is how staff cook an order twice.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal struct PdReprintOptions
{
    public PdJobOptions Job;
    public int SuppressBanner;
}

// --- M16: custom method registration (docs/api.md §16) --------------------------------
//
// The function-pointer fields are raw nint for the same reason PdTransportVtable's are:
// it keeps the struct blittable and makes the delegate lifetime explicit at the call site.
// The core copies each struct before the registration call returns, but not what the
// pointers name, so every delegate is rooted until pd_destroy.

/// <summary><c>pd_match_result</c> — a matcher's verdict plus the token it matched.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct PdMatchResult
{
    public int Kind;

    /// <summary>NUL-terminated; the core copies it out before the matcher returns.</summary>
    public fixed byte Token[8];
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdCompletionMethod
{
    public nint Id;
    public nint FenceBytes;
    public nint Matcher;
    public nint Context;
    public int Grade;
    public int Authority;
    public nint MethodName;
}

/// <summary><c>pd_probe_finding</c> — what one custom probe step concluded.</summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct PdProbeFinding
{
    public int Answered;
    public fixed byte Label[64];
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdProbeStep
{
    public nint Id;
    public nint RequestBytes;
    public nuint RequestSize;
    public nint Classify;
    public nint Context;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdBlockHandler
{
    public nint Kind;
    public nint Handler;
    public nint Context;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdFormatter
{
    public nint Name;
    public nint Formatter;
    public nint Context;
}

[StructLayout(LayoutKind.Sequential)]
internal struct PdDrawerKickReg
{
    public nint Id;
    public nint KickBytes;
    public nint StatusRequest;
    public nint StatusParse;
    public nint Context;
}
