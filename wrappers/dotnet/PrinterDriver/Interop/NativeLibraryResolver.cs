using System.Reflection;
using System.Runtime.InteropServices;

namespace PrinterDriver;

/// <summary>
/// Finds the shared library that exports the <c>pd_*</c> ABI and hands it to the
/// P/Invoke layer.
/// </summary>
/// <remarks>
/// <para>Resolution order, first hit wins:</para>
/// <list type="number">
///   <item><description>the <c>PRINTERDRIVER_LIB_PATH</c> environment variable, naming
///   either the library file itself or a directory containing it. An explicit path is an
///   instruction, not a hint: when it is set and cannot be opened, resolution stops
///   rather than silently binding some other build of the SDK;</description></item>
///   <item><description><c>runtimes/&lt;rid&gt;/native/</c> beside the managed assembly,
///   which is the NuGet layout and where a hand-staged deployment normally puts
///   it;</description></item>
///   <item><description>the platform loader's own search, which is what resolves the
///   library when it arrives through the NuGet package's own runtime asset
///   handling.</description></item>
/// </list>
/// </remarks>
public static class NativeLibraryResolver
{
    /// <summary>Environment variable that overrides every other lookup.</summary>
    public const string PathVariable = "PRINTERDRIVER_LIB_PATH";

    private static readonly object Gate = new();
    private static readonly HashSet<Assembly> Registered = [];

    /// <summary>The file the resolver actually opened, or null before the first call.</summary>
    public static string? ResolvedPath { get; private set; }

    /// <summary>
    /// Installs the resolver for another assembly's <c>DllImport</c>s. The test suite uses
    /// this so that its own declarations of the test-only entry points
    /// (<c>pd_add_printer_scripted</c> and the enum bridge) resolve to the same library.
    /// Calling it twice for the same assembly is a no-op.
    /// </summary>
    /// <param name="assembly">The assembly whose P/Invokes should use this resolver.</param>
    public static void RegisterFor(Assembly assembly)
    {
        ArgumentNullException.ThrowIfNull(assembly);
        lock (Gate)
        {
            if (!Registered.Add(assembly))
            {
                return;
            }
            NativeLibrary.SetDllImportResolver(assembly, Resolve);
        }
    }

    /// <summary>The platform's file name for the C ABI shared library.</summary>
    public static string DefaultFileName
    {
        get
        {
            if (OperatingSystem.IsWindows())
            {
                return "printerdriver_capi.dll";
            }
            if (OperatingSystem.IsMacOS())
            {
                return "libprinterdriver_capi.dylib";
            }
            return "libprinterdriver_capi.so";
        }
    }

    private static nint Resolve(string libraryName, Assembly assembly, DllImportSearchPath? path)
    {
        if (libraryName != NativeMethods.LibraryName)
        {
            return 0;
        }

        var fromEnvironment = Environment.GetEnvironmentVariable(PathVariable);
        if (!string.IsNullOrEmpty(fromEnvironment))
        {
            var candidate = Directory.Exists(fromEnvironment)
                ? Path.Combine(fromEnvironment, DefaultFileName)
                : fromEnvironment;
            if (NativeLibrary.TryLoad(candidate, out var explicitHandle))
            {
                ResolvedPath = candidate;
                return explicitHandle;
            }
            throw new PrinterDriverException(
                $"{PathVariable} is set to '{fromEnvironment}' but no PrinterDriver native " +
                $"library could be opened there. Unset it, or point it at the built " +
                $"{DefaultFileName}.");
        }

        var beside = Path.GetDirectoryName(assembly.Location);
        if (!string.IsNullOrEmpty(beside))
        {
            var rid = RuntimeInformation.RuntimeIdentifier;
            var layout = Path.Combine(beside, "runtimes", rid, "native", DefaultFileName);
            if (File.Exists(layout) && NativeLibrary.TryLoad(layout, out var layoutHandle))
            {
                ResolvedPath = layout;
                return layoutHandle;
            }
        }

        // Nothing found by name: hand the decision back to the default probing logic,
        // which is what resolves the library when NuGet's own runtime asset handling has
        // already staged it.
        if (NativeLibrary.TryLoad(libraryName, assembly, path, out var handle))
        {
            ResolvedPath = libraryName;
            return handle;
        }
        return 0;
    }
}
