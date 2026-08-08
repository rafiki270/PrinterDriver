import 'dart:ffi';
import 'dart:io';

/// Thrown when the native PrinterDriver library cannot be found or opened.
final class PrinterDriverLibraryNotFound implements Exception {
  PrinterDriverLibraryNotFound(this.message, this.attempted);

  final String message;

  /// Every path that was tried, in order, so a deployment mistake is diagnosable from
  /// the exception alone.
  final List<String> attempted;

  @override
  String toString() => 'PrinterDriverLibraryNotFound: $message\n'
      'Tried:\n${attempted.map((path) => '  $path').join('\n')}\n'
      'Set PRINTERDRIVER_LIB_PATH to the built library, or pass libraryPath to '
      'PrinterDriver.open().';
}

/// The environment variable that overrides every other lookup.
const String printerDriverLibPathVariable = 'PRINTERDRIVER_LIB_PATH';

/// The platform's file name for the C ABI shared library.
String get defaultLibraryFileName {
  if (Platform.isMacOS || Platform.isIOS) return 'libprinterdriver_capi.dylib';
  if (Platform.isWindows) return 'printerdriver_capi.dll';
  return 'libprinterdriver_capi.so';
}

/// Opens the shared library that exports the `pd_*` ABI.
///
/// Resolution order, first hit wins:
///
///  1. [path], when the caller knows exactly which artifact to bind;
///  2. the `PRINTERDRIVER_LIB_PATH` environment variable, which may name either a file
///     or a directory containing one;
///  3. the directory holding the running executable, and a `lib/` beside it — where an
///     application that ships the library normally puts it;
///  4. the plain library name, letting the platform loader search its own paths
///     (`DYLD_LIBRARY_PATH`, `LD_LIBRARY_PATH`, the app bundle, `/usr/local/lib`).
///
/// On iOS and on Android the library is normally linked into the application binary
/// rather than loaded from a file; pass `process: true` for that case.
DynamicLibrary loadPrinterDriverLibrary({String? path, bool process = false}) {
  if (process) return DynamicLibrary.process();

  final attempted = <String>[];

  DynamicLibrary? tryOpen(String candidate) {
    attempted.add(candidate);
    try {
      return DynamicLibrary.open(candidate);
    } on ArgumentError {
      return null;
    } on Exception {
      return null;
    }
  }

  final fileName = defaultLibraryFileName;

  // An explicit path is an instruction, not a hint: falling back to a library found
  // somewhere else would mean an application silently binding a different build of the
  // SDK than the one it named.
  if (path != null && path.isNotEmpty) {
    final library = tryOpen(_resolveAgainstDirectory(path, fileName));
    if (library != null) return library;
    throw PrinterDriverLibraryNotFound(
      'the library named explicitly could not be opened',
      attempted,
    );
  }

  for (final candidate in _candidatePaths(fileName)) {
    final library = tryOpen(candidate);
    if (library != null) return library;
  }

  throw PrinterDriverLibraryNotFound(
    'no PrinterDriver native library ($fileName) could be opened',
    attempted,
  );
}

/// A directory is as useful a thing to point at as a file, and getting that wrong is
/// the most common deployment mistake, so both are accepted.
String _resolveAgainstDirectory(String root, String fileName) =>
    Directory(root).existsSync()
        ? '$root${Platform.pathSeparator}$fileName'
        : root;

Iterable<String> _candidatePaths(String fileName) sync* {
  final fromEnvironment = Platform.environment[printerDriverLibPathVariable];
  if (fromEnvironment != null && fromEnvironment.isNotEmpty) {
    yield _resolveAgainstDirectory(fromEnvironment, fileName);
  }

  final executableDirectory = File(Platform.resolvedExecutable).parent.path;
  yield '$executableDirectory${Platform.pathSeparator}$fileName';
  yield '$executableDirectory${Platform.pathSeparator}lib'
      '${Platform.pathSeparator}$fileName';

  yield fileName;
}
