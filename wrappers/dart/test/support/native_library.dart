import 'dart:ffi';
import 'dart:io';

import 'package:printerdriver/printerdriver.dart';
import 'package:printerdriver/src/bindings.dart';

/// Locating the native library the suite binds.
///
/// The tests drive `libprinterdriver_capi_testing`, not the library that ships: it is
/// the same C ABI plus the scripted device and the enum bridge of
/// `capi/tests/pd_test_support.h`. Build it from the repository root with
///
/// ```sh
/// cmake -S . -B build-dart -DPD_BUILD_SHARED_CAPI=ON
/// cmake --build build-dart --target printerdriver_capi_testing
/// ```
///
/// and either let this helper find it or name it explicitly with
/// `PRINTERDRIVER_LIB_PATH`.
String get testingLibraryFileName {
  if (Platform.isMacOS) return 'libprinterdriver_capi_testing.dylib';
  if (Platform.isWindows) return 'printerdriver_capi_testing.dll';
  return 'libprinterdriver_capi_testing.so';
}

/// The path the suite loads, or null when nothing was built.
String? resolveTestingLibrary() {
  final override = Platform.environment[printerDriverLibPathVariable];
  if (override != null && override.isNotEmpty) {
    if (FileSystemEntity.isDirectorySync(override)) {
      final inDirectory =
          '$override${Platform.pathSeparator}$testingLibraryFileName';
      if (File(inDirectory).existsSync()) return inDirectory;
    } else if (File(override).existsSync()) {
      return override;
    }
  }

  // Walk up from the package towards the repository root, trying the build directories
  // the README tells people to use.
  var directory = Directory.current.absolute;
  for (var depth = 0; depth < 6; depth++) {
    for (final buildDirectory in const [
      'build-dart',
      'build',
      'build-m4',
      'out'
    ]) {
      final candidate = <String>[
        directory.path,
        buildDirectory,
        testingLibraryFileName,
      ].join(Platform.pathSeparator);
      if (File(candidate).existsSync()) return candidate;
    }
    final parent = directory.parent;
    if (parent.path == directory.path) break;
    directory = parent;
  }
  return null;
}

/// The reason to skip, or null when the suite can run.
String? get skipReasonWhenLibraryMissing => resolveTestingLibrary() == null
    ? 'no $testingLibraryFileName found: configure with '
        '`cmake -S . -B build-dart -DPD_BUILD_SHARED_CAPI=ON` and build the '
        'printerdriver_capi_testing target, or set $printerDriverLibPathVariable'
    : null;

/// The path of the library that ships — the one with no test doubles in it — or null
/// when it has not been built.
String? resolveShippedLibrary() {
  var directory = Directory.current.absolute;
  for (var depth = 0; depth < 6; depth++) {
    for (final buildDirectory in const [
      'build-dart',
      'build',
      'build-m4',
      'out'
    ]) {
      final candidate = <String>[
        directory.path,
        buildDirectory,
        defaultLibraryFileName,
      ].join(Platform.pathSeparator);
      if (File(candidate).existsSync()) return candidate;
    }
    final parent = directory.parent;
    if (parent.path == directory.path) break;
    directory = parent;
  }
  return null;
}

DynamicLibrary? _library;

/// The loaded testing library, opened once for the whole suite.
DynamicLibrary get testingLibrary {
  final path = resolveTestingLibrary();
  if (path == null) {
    throw StateError('the testing library has not been built');
  }
  return _library ??= loadPrinterDriverLibrary(path: path);
}

/// Raw bindings, for the tests that check the ABI itself rather than the wrapper.
PrinterDriverBindings get testingBindings =>
    PrinterDriverBindings(testingLibrary);

/// A driver with an in-memory journal: nothing under test here needs recovery, and
/// nothing should leave files behind.
PrinterDriver openTestDriver() =>
    PrinterDriver.open(fsyncDisabled: true, library: testingLibrary);
