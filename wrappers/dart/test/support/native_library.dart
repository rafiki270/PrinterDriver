import 'dart:ffi';
import 'dart:io';

import 'package:printerdriver/printerdriver.dart';
import 'package:printerdriver/src/allocation.dart';
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

// --- M15: loopback listeners (docs/api.md §15) ---------------------------------------
//
// `pd_discover` and `pd_auto_detect` sweep addresses, so there is no printer handle to
// hang a scripted transport on: they need a real socket on the far side. The fixture is
// in capi/tests/pd_test_support.cpp, next to the scripted device it pumps, and is
// reached here directly from the testing library rather than through the wrapper — the
// wrapper an application ships must not carry a test double.

typedef _StartNative = Pointer<Void> Function(Pointer<Char>);
typedef _PortNative = Uint16 Function(Pointer<Void>);
typedef _BytesNative = Size Function(Pointer<Void>);
typedef _VoidNative = Void Function(Pointer<Void>);

/// An in-process ESC/POS printer on an ephemeral loopback port.
final class ScriptedListener {
  ScriptedListener._(this._handle, this.port);

  /// [script] is `ok` (answers DLE EOT, GS I, GS ( H and GS r 1) or `silent` (accepts
  /// the connection and answers nothing at all).
  factory ScriptedListener.start(String script) {
    final start = testingLibrary.lookupFunction<_StartNative,
        Pointer<Void> Function(Pointer<Char>)>('pd_test_listener_start');
    return Arena.using((arena) {
      final handle = start(arena.string(script));
      if (handle == nullptr) {
        throw StateError('the scripted listener $script was refused');
      }
      final port = testingLibrary.lookupFunction<_PortNative,
          int Function(Pointer<Void>)>('pd_test_listener_port')(handle);
      return ScriptedListener._(handle, port);
    });
  }

  final Pointer<Void> _handle;

  /// The loopback port it bound.
  final int port;

  String get endpoint => '127.0.0.1:$port';

  /// How many printable bytes ever reached the device behind it — the number a
  /// detection test has to watch stay at zero.
  int get printDataBytes => testingLibrary
      .lookupFunction<_BytesNative, int Function(Pointer<Void>)>(
    'pd_test_listener_print_data_bytes',
  )(_handle);

  /// Closes the socket without destroying the handle, so a test can turn a listener into
  /// a refused port at a known address.
  void stop() => testingLibrary
      .lookupFunction<_VoidNative, void Function(Pointer<Void>)>(
    'pd_test_listener_stop',
  )(_handle);

  void destroy() => testingLibrary
      .lookupFunction<_VoidNative, void Function(Pointer<Void>)>(
    'pd_test_listener_destroy',
  )(_handle);
}
