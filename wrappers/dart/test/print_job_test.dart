/// End-to-end behaviour through the ABI, against the scripted devices of
/// `capi/tests/pd_test_support.h`.
///
/// These are the same scenarios `capi/tests/test_capi.c` covers at the C level, run
/// through the Dart surface an application actually uses, so that a wrapper mistake —
/// a dropped event, a collapsed tri-state, a dedupe that returns a different object —
/// fails here rather than in a kitchen.
library;

import 'dart:async';
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

      // The evidence of docs/api.md §13 rides on the result itself: never a bare
      // success, always what class of evidence it is, who said so and which command
      // produced it.
      final done = results['ok']! as JobDone;
      expect(done.grade, ConfidenceGrade.aJobLevelConfirmation);
      expect(done.grade.letter, 'A');
      expect(done.authority, CompletionAuthority.physicalPrinter);
      expect(done.method, 'GS(H) fn48');
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

    test('events arrive one transition at a time, not as a batch at the end',
        () async {
      // pd.h hands the event over by value, so a NativeCallable.listener can read it
      // after the native call returned. That is what makes this live: the stream must
      // deliver states while the job is still moving, not only once it settles.
      final printer = driver.addScriptedPrinterForTesting(script: 'silent');
      final job = printer.print(
        rawText('LIVE PROGRESS'),
        options: const JobOptions(key: 'dart-live-1'),
      );

      final seen = <JobState>[];
      final firstSendStarted = Completer<void>();
      final subscription = job.events.listen((event) {
        seen.add(event.state);
        if (event.state == JobState.sendStarted &&
            !firstSendStarted.isCompleted) {
          firstSendStarted.complete();
        }
      });

      // The silent device never acknowledges, so the job cannot be terminal yet: any
      // event observed here was observed mid-flight.
      await firstSendStarted.future;
      expect(job.isTerminal, isFalse,
          reason: 'the job is still waiting for a completion that never comes');
      expect(seen, contains(JobState.queued));
      expect(seen, contains(JobState.sendStarted));

      expect(await job.result, isA<JobUnknown>());
      await subscription.asFuture<void>();
      expect(seen.last, JobState.unknown);
      // Ordered, complete, and closed on the terminal event.
      expect(seen, <JobState>[
        JobState.queued,
        JobState.preflightOk,
        JobState.sendStarted,
        JobState.bytesSent,
        JobState.unknown,
      ]);
    });

    test('the receipt carries its verification id and resolves back to the job',
        () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'ok');
      final device = printer.scriptedDeviceForTesting!;
      final job = printer.print(
        rawText('VERIFY ME'),
        options: const JobOptions(key: 'dart-rvi-1'),
      );
      expect(await job.result, isA<JobDone>());

      final printToken = job.printToken!;
      final cutToken = job.cutToken!;
      expect(printToken, hasLength(4));
      expect(cutToken, hasLength(4));
      expect(printToken, isNot(cutToken));
      // [2-char instance nonce][2-char sequence]: both name this driver instance.
      expect(driver.instanceNonce, hasLength(2));
      expect(printToken.startsWith(driver.instanceNonce), isTrue);
      expect(cutToken.startsWith(driver.instanceNonce), isTrue);

      expect(device.received('ORDER: dart-rvi-1  V:$printToken'), isTrue);
      expect(identical(driver.jobByToken(printToken), job), isTrue);
      expect(identical(driver.jobByToken(cutToken), job), isTrue);
      expect(driver.jobByToken('!!!!'), isNull);
    });

    test('suppressing the verification id removes the ink, not the evidence',
        () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'ok');
      final device = printer.scriptedDeviceForTesting!;
      final job = printer.print(
        rawText('NO CODE ON THIS ONE'),
        options: const JobOptions(
          key: 'dart-rvi-quiet',
          printsVerificationId: false,
        ),
      );
      expect(await job.result, isA<JobDone>());

      final token = job.printToken!;
      expect(device.received('V:$token'), isFalse);
      expect(device.received('ORDER: dart-rvi-quiet'), isFalse);
      expect(identical(driver.jobByToken(token), job), isTrue);
    });

    test('a printer without a process-id fence has no identifier to print',
        () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'gsr1');
      final job = printer.print(
        rawText('QUEUED FENCE ONLY'),
        options: const JobOptions(key: 'dart-rvi-none'),
      );
      expect(await job.result, isA<JobDone>());
      // The identifier *is* the wire token, so a printer with no wire token has none.
      expect(job.printToken, isNull);
      expect(job.cutToken, isNull);
      expect(printer.scriptedDeviceForTesting!.received('V:'), isFalse);
    });

    test('the reprint banner is on by default and off only when asked',
        () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'ok');
      final device = printer.scriptedDeviceForTesting!;
      const key = 'dart-customer-copy';

      expect(
        await printer.send(rawText('ORIGINAL'), key: key),
        isA<JobDone>(),
      );

      final quiet = printer.forceReprint(
        key,
        options: const ReprintOptions(banner: false),
      );
      expect(await quiet.result, isA<JobDone>());
      // Still recorded as a duplicate; simply not announced on the paper.
      expect(quiet.attempt, 2);
      expect(device.received('*** REPRINT / POSSIBLE DUPLICATE ***'), isFalse);

      final loud = printer.forceReprint(key);
      expect(await loud.result, isA<JobDone>());
      expect(device.received('*** REPRINT / POSSIBLE DUPLICATE ***'), isTrue);
      expect(device.received('PRINT ATTEMPT: 3'), isTrue);
    });

    test('margins widen the clearance around the print but never narrow it',
        () async {
      final printer = driver.addScriptedPrinterForTesting(script: 'ok');
      final device = printer.scriptedDeviceForTesting!;
      final job = printer.print(
        rawText('MARGIN TICKET'),
        options: const JobOptions(
          key: 'dart-margins-1',
          topFeedDots: 40,
          bottomFeedDots: 8,
        ),
      );
      expect(await job.result, isA<JobDone>());

      // ESC J 40 for the top margin; the profile's 120-dot blade clearance survives a
      // bottom margin that asked for less than it.
      expect(device.received('\x1BJ\x28'), isTrue);
      expect(device.received('\x1BJ\x78'), isTrue);
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

    test('a profile says where its completion claim comes from', () {
      // docs/compatibility-brief.md §28. Provenance is not a confidence level and
      // changes nothing about what a job reports: it answers the earlier question of
      // whether a fence can be trusted on the strength of a profile alone. Epson is the
      // only family whose shipped defaults say Documented — everyone else's "ESC/POS
      // compatible" is a datasheet claim nobody has checked, which is exactly the
      // reading that makes probing a site's hardware worth the trip.
      const expected = <String, (Provenance, CommandLanguage)>{
        'epson_tm_t88vi': (Provenance.documented, CommandLanguage.escPos),
        'epson_tm_p20ii': (Provenance.documented, CommandLanguage.escPos),
        'xp-s260m': (Provenance.probed, CommandLanguage.escPos),
        'xprinter_s_series': (Provenance.unverified, CommandLanguage.escPos),
        'rongta_rp80': (Provenance.unverified, CommandLanguage.escPos),
        'partner_rp110': (Provenance.unverified, CommandLanguage.escPos),
        'generic_unknown': (Provenance.unverified, CommandLanguage.escPos),
        // Not ESC/POS at any level, and the profile says so before a job is submitted
        // rather than after a roll has been spooled with the wrong language.
        'zebra_zq600_plus': (Provenance.unverified, CommandLanguage.zpl),
        'brother_rj4000': (
          Provenance.unverified,
          CommandLanguage.brotherRaster
        ),
      };

      expected.forEach((profileId, answer) {
        final (provenance, language) = answer;
        // Nothing is dialled until a job needs the link, so this reads profile data
        // without touching the network.
        final printer =
            driver.addTcpPrinter(host: '127.0.0.1', profileId: profileId);
        expect(printer.completionProvenance, provenance,
            reason: '$profileId provenance');
        expect(printer.language, language, reason: '$profileId language');
      });
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
