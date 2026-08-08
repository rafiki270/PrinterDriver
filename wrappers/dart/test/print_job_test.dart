/// End-to-end behaviour through the ABI, against the scripted devices of
/// `capi/tests/pd_test_support.h`.
///
/// These are the same scenarios `capi/tests/test_capi.c` covers at the C level, run
/// through the Dart surface an application actually uses, so that a wrapper mistake —
/// a dropped event, a collapsed tri-state, a dedupe that returns a different object —
/// fails here rather than in a kitchen.
library;

import 'dart:convert';
import 'dart:typed_data';

import 'package:printerdriver/printerdriver.dart';
import 'package:test/test.dart';

import 'support/native_library.dart';

Payload rawText(String text) =>
    Payload.raw(Uint8List.fromList(utf8.encode(text)));

void main() {
  final skip = skipReasonWhenLibraryMissing;

  group('print job', () {
    late PrinterDriver driver;

    setUp(() => driver = openTestDriver());
    tearDown(() => driver.dispose());

    test('a submitted job reaches a terminal Done on a healthy device',
        () async {
      final printer = driver.addScriptedPrinterForTesting(
        script: 'ok',
        printerId: 'dart-e2e',
      );
      final device = printer.scriptedDeviceForTesting!;

      final job = printer.print(
        rawText('END TO END VIA DART'),
        options: const JobOptions(key: 'dart-e2e-1'),
      );

      final result = await job.result;

      expect(result, isA<JobDone>());
      // A GS ( H printer that also answers the post-cut cutter read: the claim is
      // allowed to reach the top of the ladder, and not one rung higher.
      expect(result.confidence, ConfidenceLevel.cutFaultFree);
      expect(job.isTerminal, isTrue);
      expect(job.currentState, JobState.doneSoftware);
      expect(job.key, 'dart-e2e-1');
      expect(job.attempt, 1);
      expect(device.received('END TO END VIA DART'), isTrue);
      expect(device.cuts, 1);
    });

    test(
        'a job on a GS r 1 printer stops at the confidence that fence supports',
        () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'gsr1');
      expect(printer.completion, CompletionMechanism.gsR1);

      final result = await printer.send(rawText('ORDERED FENCE ONLY'));

      expect(result, isA<JobDone>());
      expect(result.confidence, ConfidenceLevel.cutProcessed);
    });

    test('the same key returns the identical job and prints once', () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'ok');
      final device = printer.scriptedDeviceForTesting!;
      const options = JobOptions(key: 'dart-dedupe-1');

      final first = printer.print(rawText('ONE COPY ONLY'), options: options);
      final second =
          printer.print(rawText('SHOULD NEVER PRINT'), options: options);

      // pd.h returns the same pd_job handle, and this wrapper interns handles, so
      // dedupe is visible as Dart identity rather than as an equal id.
      expect(identical(first, second), isTrue);
      expect(second.id, first.id);

      expect(await first.result, isA<JobDone>());
      printer.drain();

      expect(device.received('ONE COPY ONLY'), isTrue);
      expect(device.received('SHOULD NEVER PRINT'), isFalse);
      expect(device.cuts, 1);

      // The driver's own key index resolves to the identical object too.
      expect(identical(driver.findJob('dart-dedupe-1'), first), isTrue);
      expect(driver.findJob('no-such-key'), isNull);
    });

    test('the event stream is the recorded state progression, in order',
        () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'ok');
      final job = printer.print(
        rawText('STATE ORDER'),
        options: const JobOptions(key: 'dart-events-1'),
      );

      final states = await job.events.map((event) => event.state).toList();

      expect(states, <JobState>[
        JobState.queued,
        JobState.preflightOk,
        JobState.sendStarted,
        JobState.bytesSent,
        JobState.printConfirmed,
        JobState.cutCommandProcessed,
        JobState.doneSoftware,
      ]);

      final events = await job.events.toList();
      // Confidence only ever climbs, and never past what the last event claims.
      expect(
        events.map((event) => event.confidence.nativeValue),
        _isNonDecreasing,
      );
      expect(events.last.confidence, (await job.result).confidence);
      // A non-failure transition carries no reason at all, rather than a "none" that a
      // caller could mistake for information.
      expect(events.every((event) => event.reason == null), isTrue);
      expect(events.first.monotonicMs, greaterThan(0));

      // Replayable: the history is a record, not a one-shot subscription.
      expect(await job.events.length, states.length);
    });

    test('onProgress sees every event in order and the closure form ends once',
        () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'ok');
      final seen = <JobState>[];
      final terminal = <JobResult>[];

      final result = await printer.send(
        Payload.document(<DocumentOp>[
          DocumentOp.align(Alignment.center),
          DocumentOp.bold(true),
          DocumentOp.line('TABLE 12'),
          DocumentOp.bold(false),
          DocumentOp.align(Alignment.left),
          DocumentOp.line('1x Flat white'),
          DocumentOp.feed(2),
        ]),
        key: 'order-7F3A-92C1#kitchen-1',
        onProgress: (event) => seen.add(event.state),
        onResult: terminal.add,
      );

      expect(seen.first, JobState.queued);
      expect(seen.last, JobState.doneSoftware);
      expect(terminal, hasLength(1));
      expect(identical(terminal.single, result), isTrue);
      expect(
        printer.scriptedDeviceForTesting!.received('1x Flat white'),
        isTrue,
      );
    });

    test('a device that never acknowledges ends Unknown, not failed', () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'silent');
      final device = printer.scriptedDeviceForTesting!;

      final result = await printer.send(rawText('NO ANSWER EVER'));

      // The bytes reached the paper path; nothing confirmed them. Collapsing this into
      // either bucket is the bug the tri-state exists to prevent.
      expect(result, isA<JobUnknown>());
      expect((result as JobUnknown).reason,
          FailureReason.timeoutAwaitingCompletion);
      expect(device.printDataBytes, greaterThan(0));
    });

    test('strict preflight refuses a paper-out device before any payload byte',
        () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'paperout');
      final device = printer.scriptedDeviceForTesting!;

      final result = await printer.send(rawText('MUST NOT BE SENT'));

      expect(result, isA<JobFailed>());
      expect((result as JobFailed).reason, FailureReason.preflightPaperOut);
      expect(device.printDataBytes, 0);
      expect(device.cuts, 0);
    });

    test('an unreachable transport fails rather than reporting an unknown',
        () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'refuse');

      final result = await printer.send(rawText('NOBODY HOME'));

      expect(result, isA<JobFailed>());
      expect((result as JobFailed).reason, FailureReason.transportUnreachable);
      expect(result.confidence, ConfidenceLevel.transportAccepted);
    });

    test(
        'the terminal result is a tri-state a switch has to cover exhaustively',
        () async {
      final results = <String, JobResult>{
        'ok': await driver
            .addScriptedPrinterForTesting(script: 'ok')
            .send(rawText('DONE')),
        'silent': await driver
            .addScriptedPrinterForTesting(script: 'silent')
            .send(rawText('UNKNOWN')),
        'paperout': await driver
            .addScriptedPrinterForTesting(script: 'paperout')
            .send(rawText('FAILED')),
      };

      // No `default`, no boolean: this switch is exhaustive because JobResult is sealed
      // and has exactly the three cases of docs/api.md §4. Adding a fourth outcome to
      // the ABI would stop this compiling, which is the point.
      String decide(JobResult result) => switch (result) {
            JobDone(:final confidence) => 'printed at ${confidence.name}',
            JobFailed(:final reason) => 'failed: ${reason.name}',
            JobUnknown(:final reason) => 'ask an operator: ${reason.name}',
          };

      expect(decide(results['ok']!), 'printed at cutFaultFree');
      expect(decide(results['paperout']!), 'failed: preflightPaperOut');
      expect(
        decide(results['silent']!),
        'ask an operator: timeoutAwaitingCompletion',
      );

      // The evidence fields of docs/api.md §13 are not carried by pd_job_result at this
      // ABI revision, and this wrapper reports that rather than inventing a grade.
      final done = results['ok']! as JobDone;
      expect(done.grade, isNull);
      expect(done.authority, isNull);
      expect(done.method, isNull);
    });

    test(
        'forceReprint prints a second copy and marks it as a possible duplicate',
        () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'ok');
      final device = printer.scriptedDeviceForTesting!;
      const key = 'order-7F3A-92C1#kitchen-1';

      final original = printer.print(
        rawText('ORIGINAL TICKET'),
        options: const JobOptions(key: key),
      );
      expect(await original.result, isA<JobDone>());

      final reprint = printer.forceReprint(key);
      expect(identical(reprint, original), isFalse);
      expect(await reprint.result, isA<JobDone>());
      expect(reprint.attempt, 2);

      expect(device.cuts, 2);
      expect(device.received('*** REPRINT / POSSIBLE DUPLICATE ***'), isTrue);
      expect(device.received('PRINT ATTEMPT: 2'), isTrue);

      // An unknown key has nothing to reprint, and says so instead of printing.
      expect(
        () => printer.forceReprint('never-submitted'),
        throwsA(isA<PrinterDriverException>()),
      );
    });

    test('the device event stream reports what the printer said', () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'ok');
      final events = <DeviceEvent>[];
      final subscription = printer.events.listen(events.add);
      addTearDown(subscription.cancel);

      await printer.send(rawText('WATCH THE DEVICE'));
      // Device events are delivered live, on the thread that decoded the status, so
      // give the isolate a turn to drain them.
      await Future<void>.delayed(const Duration(milliseconds: 50));

      expect(events, isNotEmpty);
      expect(events, everyElement(isNot(DeviceEvent.unrecognized)));
      expect(events, contains(DeviceEvent.online));
    });

    test('status is a snapshot that admits what it has not observed', () {
      final printer = driver.addScriptedPrinterForTesting(script: 'ok');

      final before = printer.status;
      expect(before.observed, isFalse);
      // Never heard from: tri-states are null rather than a cheerful default.
      expect(before.online, isNull);
      expect(before.paperOut, isNull);

      final refreshed =
          printer.refreshStatus(timeout: const Duration(seconds: 2));
      expect(refreshed.observed, isTrue);
      expect(refreshed.paperOut, isFalse);
      expect(refreshed.coverOpen, isFalse);
    });

    test('printers report the width and the fence a profile actually answers',
        () {
      final printer = driver.addScriptedPrinterForTesting(
        script: 'ok',
        printerId: 'kitchen-1',
      );

      expect(printer.id, 'kitchen-1');
      expect(printer.widthDots, 576);
      expect(printer.completion, CompletionMechanism.gsParenH);
      expect(driver.profileIds, contains('generic'));
      expect(driver.lastError, isEmpty);
    });

    test(
        'disposing while a job is in flight reports that, rather than an outcome',
        () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'silent');
      final job = printer.print(rawText('IN FLIGHT'));
      final pending = job.result;

      // pd_destroy waits for the job to reach a terminal state and then frees every
      // handle, so the answer exists but is no longer readable. Saying so is the only
      // honest option: a printing outcome invented here is exactly the lie this SDK
      // was built to remove.
      driver.dispose();

      await expectLater(pending, throwsStateError);
      await expectLater(job.events.toList(), throwsStateError);
    });

    test('a disposed driver refuses every further call', () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'ok');
      expect(await printer.send(rawText('LAST ONE')), isA<JobDone>());

      driver.dispose();

      expect(driver.isDisposed, isTrue);
      expect(() => printer.status, throwsStateError);
      expect(() => driver.findJob('anything'), throwsStateError);
      expect(
        () => driver.addScriptedPrinterForTesting(script: 'ok'),
        throwsStateError,
      );
      // Idempotent: a second dispose must not double-free the native driver.
      driver.dispose();
    });
  }, skip: skip);
}

final Matcher _isNonDecreasing = predicate<Iterable<int>>(
  (values) {
    var previous = values.isEmpty ? 0 : values.first;
    for (final value in values) {
      if (value < previous) return false;
      previous = value;
    }
    return true;
  },
  'never decreases',
);
