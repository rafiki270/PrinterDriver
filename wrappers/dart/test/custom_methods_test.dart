/// Custom method registration from Dart (docs/api.md §16).
///
/// `capi/tests/test_capi.c` proves the five registration points at the C level. This
/// proves the Dart surface over them, and it proves the one thing that is specific to
/// this wrapper: a registered completion method drives a whole job to a graded Done
/// through `dart:ffi`, with the fence and the matcher living in native code because a
/// Dart callback cannot answer a core thread synchronously (see
/// `lib/src/custom_methods.dart`).
///
/// The scheme is the same made-up one the other wrappers use — `acme.x-idle`: the host
/// sends `ESC 'x'` plus the job's four-character verification token behind the payload,
/// and an idle device echoes `ESC 'y'` plus the same token. Its reference implementation
/// is exported by the testing library as `pd_test_acme_fence_bytes` /
/// `pd_test_acme_matcher`, which is exactly the shape an application's own vendor plugin
/// has.
library;

import 'dart:convert';
import 'dart:typed_data';

import 'package:printerdriver/printerdriver.dart';
import 'package:test/test.dart';

import 'support/native_library.dart';

Payload rawText(String text) => Payload.raw(Uint8List.fromList(utf8.encode(text)));

CustomCompletionMethod acmeIdle() => CustomCompletionMethod.fromLibrary(
      testingLibrary,
      id: 'acme.x-idle',
      fenceBytes: 'pd_test_acme_fence_bytes',
      matcher: 'pd_test_acme_matcher',
      grade: ConfidenceGrade.aJobLevelConfirmation,
      authority: CompletionAuthority.physicalPrinter,
    );

void main() {
  final skip = skipReasonWhenLibraryMissing;

  group('custom completion method', () {
    test('a registered vendor method earns its declared grade', () async {
      final driver = openTestDriver();
      addTearDown(driver.dispose);
      driver.registerCompletionMethod(acmeIdle());

      // A VendorIdle profile bound to the id registered above, behind a device that
      // echoes the ESC x fence.
      final printer = driver.addScriptedPrinterForTesting(
          script: 'vendor-idle', printerId: 'dart-acme');
      expect(printer.completion, CompletionMechanism.vendorIdle);

      final job = printer.print(rawText('ACME IDLE TICKET'),
          options: const JobOptions(key: 'acme-1'));
      final result = await job.result;

      expect(result, isA<JobDone>());
      final done = result as JobDone;
      // The registered claim, attributed by id.
      expect(done.grade, ConfidenceGrade.aJobLevelConfirmation);
      expect(driver.gradeLetter(done.grade), 'A');
      expect(done.authority, CompletionAuthority.physicalPrinter);
      expect(done.method, 'acme.x-idle');
      // A vendor idle fence confirms print and the cut command, not a fault-free blade.
      expect(done.confidence, ConfidenceLevel.cutProcessed);

      // The custom fence promotes its per-job token to a resolvable verification
      // identifier, exactly like GS ( H.
      final token = job.printToken;
      expect(token, isNotNull);
      expect(token!.length, 4);
      expect(token, startsWith(driver.instanceNonce));
      expect(driver.jobByToken(token), isNotNull);
    }, skip: skip);

    test('a device that never idles leaves the job unknown', () async {
      final driver = openTestDriver();
      addTearDown(driver.dispose);
      driver.registerCompletionMethod(acmeIdle());

      final printer = driver.addScriptedPrinterForTesting(
          script: 'vendor-idle-busy', printerId: 'dart-acme-busy');
      final job = printer.print(rawText('ACME IDLE TICKET'),
          options: const JobOptions(
              key: 'acme-busy-1', timeout: Duration(milliseconds: 900)));

      // Bytes went out and no ack came back. Not a success and not a failure.
      expect(await job.result, isA<JobUnknown>());
    }, skip: skip);

    test('a duplicate id is refused', () {
      final driver = openTestDriver();
      addTearDown(driver.dispose);
      driver.registerCompletionMethod(acmeIdle());
      expect(() => driver.registerCompletionMethod(acmeIdle()),
          throwsA(isA<PrinterDriverException>()));
    }, skip: skip);

    test('a missing vendor symbol is refused before registration', () {
      expect(
          () => CustomCompletionMethod.fromLibrary(
                testingLibrary,
                id: 'acme.absent',
                fenceBytes: 'pd_test_acme_fence_bytes',
                matcher: 'pd_no_such_matcher',
              ),
          throwsA(isA<ArgumentError>()));
    }, skip: skip);
  });

  group('the other four registration points', () {
    test('a probe step must not be able to print', () {
      final driver = openTestDriver();
      addTearDown(driver.dispose);

      CustomProbeStep step(String id, List<int> request) => CustomProbeStep.fromLibrary(
            testingLibrary,
            id: id,
            requestBytes: request,
            // Any classify will do here: the printable-byte lint runs at registration,
            // before a step is ever executed.
            classify: 'pd_test_acme_matcher',
          );

      // "Hi" is printable, and auto-detection must never cost a venue a roll of paper.
      expect(() => driver.registerProbeStep(step('acme.printing-probe', utf8.encode('Hi'))),
          throwsA(isA<PrinterDriverException>()));

      // ESC ENQ: every byte below 0x20, so nothing it can do will mark paper.
      driver.registerProbeStep(step('acme.silent-probe', const [0x1B, 0x05]));
      expect(() => driver.registerProbeStep(step('acme.silent-probe', const [0x1B, 0x05])),
          throwsA(isA<PrinterDriverException>()));
    }, skip: skip);

    test('block handler, formatter and drawer kick register and refuse duplicates', () {
      final driver = openTestDriver();
      addTearDown(driver.dispose);

      // The symbols below are stand-ins with the right shape: what is under test is the
      // registration path, and the core copies the record without calling anything.
      CustomBlockHandler handler(String kind) => CustomBlockHandler.fromLibrary(
          testingLibrary,
          kind: kind,
          handler: 'pd_test_acme_fence_bytes');
      driver.registerBlockHandler(handler('acme.stamp'));
      expect(() => driver.registerBlockHandler(handler('acme.stamp')),
          throwsA(isA<PrinterDriverException>()));

      CustomFormatter formatter(String name) => CustomFormatter.fromLibrary(
          testingLibrary,
          name: name,
          formatter: 'pd_test_acme_fence_bytes');
      driver.registerFormatter(formatter('acme.upper'));
      expect(() => driver.registerFormatter(formatter('acme.upper')),
          throwsA(isA<PrinterDriverException>()));

      CustomDrawerKick kick(String id) => CustomDrawerKick.fromLibrary(testingLibrary,
          id: id, kickBytes: 'pd_test_acme_fence_bytes');
      driver.registerDrawerKick(kick('acme.kick'));
      expect(() => driver.registerDrawerKick(kick('acme.kick')),
          throwsA(isA<PrinterDriverException>()));
    }, skip: skip);
  });

  group('the ABI spelling of a mirrored enum', () {
    test('comes from the core, not from Dart', () {
      final driver = openTestDriver();
      addTearDown(driver.dispose);

      expect(driver.abiName(JobState.queued), 'Queued');
      expect(driver.abiName(CompletionMatchKind.matched), 'Matched');
      expect(driver.abiName(CompletionMatchKind.notMine), 'NotMine');
      expect(driver.abiName(DrainOrder.fifo), 'Fifo');
      expect(driver.abiName(DrawerState.openVerified), isNotEmpty);
      expect(driver.gradeLetter(ConfidenceGrade.aJobLevelConfirmation), 'A');
      expect(() => driver.abiName('not an enum'), throwsA(isA<ArgumentError>()));
    }, skip: skip);
  });
}
