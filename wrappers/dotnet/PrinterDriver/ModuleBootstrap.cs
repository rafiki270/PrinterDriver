using System.Runtime.CompilerServices;

namespace PrinterDriver;

/// <summary>
/// Installs the native library resolver before anything in this assembly can P/Invoke.
/// </summary>
/// <remarks>
/// A static constructor on <see cref="NativeLibraryResolver"/> would not be enough: it
/// runs the first time <em>that type</em> is touched, and the first thing an application
/// touches may well be a static P/Invoke such as
/// <see cref="PrinterDriver.ProfileIds"/> — at which point the loader has already tried,
/// and failed, to find <c>printerdriver_capi</c> by its bare name. A module initializer
/// runs before any code in the assembly does, which is the only placement that closes
/// that window.
/// </remarks>
internal static class ModuleBootstrap
{
    // CA2255 says module initializers belong in application code, not libraries. That
    // advice exists to stop libraries running arbitrary work at load time and surprising
    // the host. This does one thing -- register a DllImportResolver for this assembly's
    // own P/Invokes -- which is the documented use for a native-interop library and
    // cannot be done correctly any later. Suppressed knowingly, not by reflex.
#pragma warning disable CA2255
    [ModuleInitializer]
#pragma warning restore CA2255
    internal static void Initialize() =>
        NativeLibraryResolver.RegisterFor(typeof(ModuleBootstrap).Assembly);
}
