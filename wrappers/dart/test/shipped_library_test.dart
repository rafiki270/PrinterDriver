/// The library an application ships, as opposed to the one the rest of this suite
/// drives.
///
/// `printerdriver_capi_shared` and `printerdriver_capi_testing` are two CMake targets
/// over the same ABI, and the split is only worth anything if it is checked: the shipped
/// one must export the whole of pd.h and none of `capi/tests/pd_test_support.h`.
library;

import 'package:printerdriver/printerdriver.dart';
import 'package:test/test.dart';

import 'support/native_library.dart';

void main() {
  final path = resolveShippedLibrary();
  final skip = path == null
      ? 'no $defaultLibraryFileName found: build the printerdriver_capi_shared target'
      : null;

  group('the shipped library', () {
    test('binds the whole ABI and carries no scripted device', () {
      final driver = PrinterDriver.open(libraryPath: path);
      addTearDown(driver.dispose);

      // Every pd_* symbol resolved, or PrinterDriver.open would already have thrown.
      expect(driver.profileIds, containsAll(<String>['generic', 'xp-s260m']));

      // Nothing is dialled until a job needs the transport, so this says nothing about
      // the network — only that the ABI accepted the configuration.
      final printer = driver.addTcpPrinter(
        host: '192.0.2.10',
        printerId: 'unreachable-by-rfc5737',
        widthDots: 576,
      );
      expect(printer.id, 'unreachable-by-rfc5737');
      expect(printer.widthDots, 576);
      expect(printer.completion, isNot(CompletionMechanism.unrecognized));
      expect(printer.status.observed, isFalse);
      expect(printer.scriptedDeviceForTesting, isNull);

      expect(
        () => driver.addScriptedPrinterForTesting(script: 'ok'),
        throwsA(
          isA<PrinterDriverException>().having(
            (error) => error.message,
            'message',
            contains('no pd_add_printer_scripted'),
          ),
        ),
      );
    });

    test('rejects a printer with no host, and says why', () {
      final driver = PrinterDriver.open(libraryPath: path);
      addTearDown(driver.dispose);

      expect(
        () => driver.addTcpPrinter(host: ''),
        throwsA(
          isA<PrinterDriverException>().having(
            (error) => error.message,
            'message',
            contains('host'),
          ),
        ),
      );
    });

    test('a missing library names every path it tried', () {
      expect(
        () => PrinterDriver.open(
            libraryPath: '/nonexistent/libprinterdriver_capi.dylib'),
        throwsA(
          isA<PrinterDriverLibraryNotFound>().having(
            (error) => error.attempted,
            'attempted',
            contains('/nonexistent/libprinterdriver_capi.dylib'),
          ),
        ),
      );
    });
  }, skip: skip);
}
