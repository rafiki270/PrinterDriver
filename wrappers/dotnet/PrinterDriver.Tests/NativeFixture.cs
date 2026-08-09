using System.Runtime.InteropServices;

namespace PrinterDriver.Tests;

/// <summary>
/// Locates and binds the native library this suite drives.
/// </summary>
/// <remarks>
/// <para>
/// The tests drive <c>printerdriver_capi_testing</c>, not the library that ships: it is
/// the same C ABI plus the scripted device and the enum bridge of
/// <c>capi/tests/pd_test_support.h</c>. Build it from the repository root with
/// </para>
/// <code>
/// cmake -S . -B build-dotnet -DPD_BUILD_SHARED_CAPI=ON
/// cmake --build build-dotnet --target printerdriver_capi_testing
/// </code>
/// <para>
/// and either let this helper find it or name it explicitly with
/// <c>PRINTERDRIVER_LIB_PATH</c>.
/// </para>
/// <para>
/// There is no silent skip when it is missing. A suite that quietly passes because it
/// tested nothing is worse than one that fails: the failure message below says exactly
/// what to build.
/// </para>
/// </remarks>
internal static class NativeFixture
{
    private static readonly object Gate = new();
    private static bool _bound;

    internal static string FileName
    {
        get
        {
            if (OperatingSystem.IsWindows())
            {
                return "printerdriver_capi_testing.dll";
            }
            if (OperatingSystem.IsMacOS())
            {
                return "libprinterdriver_capi_testing.dylib";
            }
            return "libprinterdriver_capi_testing.so";
        }
    }

    /// <summary>Binds the testing library, once per process.</summary>
    internal static void Bind()
    {
        lock (Gate)
        {
            if (_bound)
            {
                return;
            }
            var path = Resolve()
                ?? throw new InvalidOperationException(
                    $"no {FileName} found. Build it from the repository root with\n" +
                    "  cmake -S . -B build-dotnet -DPD_BUILD_SHARED_CAPI=ON\n" +
                    "  cmake --build build-dotnet --target printerdriver_capi_testing\n" +
                    $"or set {NativeLibraryResolver.PathVariable} to an existing build.");

            // The wrapper's own resolver reads this variable first, so pointing it at the
            // testing build is all it takes for both assemblies to bind the same file.
            Environment.SetEnvironmentVariable(NativeLibraryResolver.PathVariable, path);
            NativeLibraryResolver.RegisterFor(typeof(TestNative).Assembly);
            NativeLibraryResolver.RegisterFor(typeof(NativeLibraryResolver).Assembly);
            _bound = true;
        }
    }

    internal static string? Resolve()
    {
        var explicitPath = Environment.GetEnvironmentVariable(NativeLibraryResolver.PathVariable);
        if (!string.IsNullOrEmpty(explicitPath))
        {
            if (Directory.Exists(explicitPath))
            {
                var inDirectory = Path.Combine(explicitPath, FileName);
                if (File.Exists(inDirectory))
                {
                    return inDirectory;
                }
            }
            else if (File.Exists(explicitPath))
            {
                return explicitPath;
            }
        }

        // Walk up from the test binary towards the repository root, trying the build
        // directories the README tells people to use.
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        for (var depth = 0; depth < 8 && directory is not null; depth++)
        {
            foreach (var build in (string[])
                     ["build-dotnet", "build", "build-baseline", "build-m4", "build-dart", "out"])
            {
                var candidate = Path.Combine(directory.FullName, build, FileName);
                if (File.Exists(candidate))
                {
                    return candidate;
                }
            }
            directory = directory.Parent;
        }
        return null;
    }
}

/// <summary>
/// Test-only entry points: the scripted device and the enum bridge of
/// <c>capi/tests/pd_test_support.h</c>. They exist in
/// <c>printerdriver_capi_testing</c> and in nothing that ships — a library with a test
/// double in it is not the library under test.
/// </summary>
internal static class TestNative
{
    private const string Library = "printerdriver_capi";
    private const CallingConvention Cdecl = CallingConvention.Cdecl;

    /// <summary>
    /// Attaches a printer whose transport is an in-process scripted device. Scripts:
    /// "ok" (healthy GS ( H printer), "gsr1" (queued fence only), "silent" (never answers
    /// the completion marker), "paperout" (strict preflight refuses), "refuse" (the
    /// connection itself fails).
    /// </summary>
    [DllImport(Library, CallingConvention = Cdecl)]
    internal static extern nint pd_add_printer_scripted(
        nint driver,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? printerId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string script);

    [DllImport(Library, CallingConvention = Cdecl)]
    internal static extern nuint pd_test_print_data_bytes(nint printer);

    [DllImport(Library, CallingConvention = Cdecl)]
    internal static extern nuint pd_test_cuts(nint printer);

    [DllImport(Library, CallingConvention = Cdecl)]
    internal static extern int pd_test_received_contains(
        nint printer, [MarshalAs(UnmanagedType.LPUTF8Str)] string needle);

    [DllImport(Library, CallingConvention = Cdecl)]
    internal static extern int pd_test_cpp_enum_count(int which);

    [DllImport(Library, CallingConvention = Cdecl)]
    internal static extern nint pd_test_cpp_enum_name(int which, int index);

    [DllImport(Library, CallingConvention = Cdecl)]
    internal static extern int pd_test_cpp_enum_value(int which, int index);

    [DllImport(Library, CallingConvention = Cdecl)]
    internal static extern nint pd_test_enum_label(int which);

    internal static string ReadUtf8(nint pointer) =>
        pointer == 0 ? string.Empty : Marshal.PtrToStringUTF8(pointer) ?? string.Empty;
}

/// <summary>The <c>pd_test_enum</c> ids of <c>capi/tests/pd_test_support.h</c>.</summary>
internal enum BridgedEnum
{
    JobState = 0,
    Confidence = 1,
    DeviceEvent = 2,
    FailureReason = 3,
    JobOutcome = 4,
    Cut = 5,
    Preflight = 6,
    PayloadKind = 7,
    Completion = 8,
    CutVariant = 9,
    Alignment = 10,
    CodePage = 11,
    Binarization = 12,
    ConfidenceGrade = 13,
    CompletionAuthority = 14,
}

/// <summary>
/// A driver with an in-memory journal: nothing under test here needs crash recovery, and
/// nothing should leave files behind.
/// </summary>
internal static class TestDriver
{
    internal static PrinterDriver Open()
    {
        NativeFixture.Bind();
        return PrinterDriver.Create(new PrinterDriverConfig(FsyncDisabled: true));
    }

    /// <summary>Attaches a scripted device and returns the wrapper's own Printer for it.</summary>
    internal static Printer AddScripted(this PrinterDriver driver, string script,
                                        string? printerId = null)
    {
        var handle = TestNative.pd_add_printer_scripted(driver.Handle, printerId, script);
        Assert.NotEqual(0, handle);
        return driver.Intern(handle);
    }
}
