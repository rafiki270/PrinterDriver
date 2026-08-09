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

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void pd_subscribe_device(nint driver, nint printer,
                                                    DeviceEventCallback callback, nint context);

    // --- Jobs ------------------------------------------------------------------------

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_print(nint driver, nint printer, in PdPayload payload,
                                         in PdJobOptions options);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_force_reprint(nint driver, nint printer,
                                                 [MarshalAs(UnmanagedType.LPUTF8Str)] string key,
                                                 in PdJobOptions options);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_find_job(nint driver,
                                            [MarshalAs(UnmanagedType.LPUTF8Str)] string key);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_job_id(nint job);

    [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern nint pd_job_key(nint job);

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
    internal static extern int pd_code_page_at(int index);

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
