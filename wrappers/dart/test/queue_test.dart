import 'dart:convert';
import 'dart:typed_data';

import 'package:printerdriver/printerdriver.dart';
import 'package:test/test.dart';

import 'support/native_library.dart';

/// M13b. The print-queue addon through the ABI (docs/sdk-spec.md §12).
///
/// One happy path, driven against the real engine over the real `pd_queue_*` surface, so
/// what is under test is the binding plus the addon rather than a rehearsal of the
/// binding's own assumptions.
void main() {
  final skip = skipReasonWhenLibraryMissing;

  group('print queue', () {
    late PrinterDriver driver;

    setUp(() => driver = openTestDriver());
    tearDown(() => driver.dispose());

    test('an enqueued job drains through the same engine and earns the same grade',
        () async {
      final printer = driver.addScriptedPrinterForTesting(
        script: 'ok',
        printerId: 'dart-queue',
      );
      final queue = PrintQueue(driver);
      addTearDown(queue.dispose);

      expect(queue.isPaused(printer.id), isFalse);
      expect(queue.isBlocked(printer.id), isFalse);

      final job = queue.enqueue(
        printer,
        Payload.raw(Uint8List.fromList(utf8.encode('QUEUED TICKET'))),
        options: const QueueOptions(key: 'dart-queued-1'),
      );
      expect(job.key, 'dart-queued-1');

      final result = await job.result;
      // Rule 3 of §12, observable: a queued job goes down the identical engine path a
      // direct print takes, so it earns the identical claim from the identical fence.
      expect(result, isA<JobDone>());
      expect(result.grade, ConfidenceGrade.aJobLevelConfirmation);
      expect(result.authority, CompletionAuthority.physicalPrinter);
      expect(result.method, 'GS(H) fn48');

      // Rule 2: the key is claimed in the driver's own index at enqueue time, so a direct
      // print of the same key finds the queued job instead of producing a second receipt.
      final deduped = printer.print(
        Payload.raw(Uint8List.fromList(utf8.encode('DUPLICATE'))),
        options: const JobOptions(key: 'dart-queued-1'),
      );
      expect(deduped.id, job.id);

      expect(queue.pending(), 0);
      expect(queue.pending(printer.id), 0);
      expect(queue.expiredCount, 0);
      expect(queue.overflowCount, 0);
      queue.tick();

      // Operator hold is independent of anything the device is reporting.
      queue.pause(printer.id);
      expect(queue.isPaused(printer.id), isTrue);
      queue.resume(printer.id);
      expect(queue.isPaused(printer.id), isFalse);
    });
  }, skip: skip);
}
