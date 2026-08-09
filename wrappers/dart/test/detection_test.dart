/// M15 — self-test, auto-detection and LAN discovery through the Dart surface
/// (docs/api.md §15).
///
/// The wrapper holds no detection logic: which provenance column governs a mechanism,
/// what a printless probe may claim and how a ticket is laid out all live in the C++
/// core. What is under test here is that none of it is lost on the way across — and that
/// a sweep which must not print still does not print.
library;

import 'package:printerdriver/printerdriver.dart';
import 'package:printerdriver/src/allocation.dart';
import 'package:printerdriver/src/bindings.dart';
import 'package:test/test.dart';

import 'support/native_library.dart';

void main() {
  final skip = skipReasonWhenLibraryMissing;

  group('self-test', () {
    late PrinterDriver driver;

    setUp(() => driver = openTestDriver());
    tearDown(() => driver.dispose());

    test('prints one ticket and reports what it proved', () {
      final bench = driver.addScriptedPrinterForTesting(
        script: 'ok',
        printerId: 'bench',
      );
      final device = bench.scriptedDeviceForTesting!;

      final result = bench.selfTest();

      // The proof is the ordinary tri-state result of the ordinary engine.
      expect(result.result, isA<JobDone>());
      final done = result.result as JobDone;
      expect(done.confidence, ConfidenceLevel.cutFaultFree);
      expect(done.grade, ConfidenceGrade.aJobLevelConfirmation);
      expect(done.authority, CompletionAuthority.physicalPrinter);
      expect(done.method, 'GS(H) fn48');

      expect(result.key, startsWith('selftest-'));
      expect(result.verificationId, isNotNull);
      expect(result.verificationId!.length, 4);
      expect(result.ticketLines.any((l) => l.contains('PRINTERDRIVER SELF-TEST')),
          isTrue);
      expect(result.ticketLines.any((l) => l.contains('CHARSET')), isTrue);

      // The bytes the device actually received.
      expect(device.received('PRINTERDRIVER SELF-TEST'), isTrue);
      expect(device.received('V:'), isTrue);
      expect(device.cuts, 1);

      // The detection report the paper carries.
      expect(result.detection.endpoint, 'bench');
      expect(result.detection.completion.mechanism, CompletionMechanism.gsParenH);
      expect(result.detection.completion.gradeCeiling,
          ConfidenceGrade.aJobLevelConfirmation);
      expect(result.detection.media.printableWidthDots, 576);
      expect(result.detection.media.charsPerLine, 48);
      expect(result.detection.degradations, isEmpty);
      expect(result.detection.provenanceSummary, contains('GS(H) fn48'));
      expect(result.detection.selection, ProfileSelection.documented);

      // A self-test is an ordinary job under an ordinary key: the same key twice
      // prints once.
      bench.selfTest(key: 'selftest-fixed');
      expect(device.cuts, 2);
      bench.selfTest(key: 'selftest-fixed');
      expect(device.cuts, 2);
    }, skip: skip);
  });

  group('auto-detection', () {
    late PrinterDriver driver;

    setUp(() => driver = openTestDriver());
    tearDown(() => driver.dispose());

    test('classifies answering, silent and refusing listeners apart', () async {
      final answering = ScriptedListener.start('ok');
      final silent = ScriptedListener.start('silent');
      final gone = ScriptedListener.start('ok');
      final refused = gone.endpoint;
      gone.stop(); // the port is now closed: a refusal, deterministically

      try {
        final found = driver.autoDetect(
          endpoints: [answering.endpoint, silent.endpoint, refused],
          connectTimeout: const Duration(milliseconds: 500),
          responseTimeout: const Duration(milliseconds: 150),
        );
        expect(found, hasLength(3));

        final talker = found.firstWhere((p) => p.status == DetectionStatus.answered);
        expect(talker.portOpen, isTrue);
        expect(talker.fromCache, isFalse);
        expect(talker.summary.identity.model, 'TM-T88V');
        // GS I is a string the firmware chooses, and at least one family ships
        // answering as somebody else's model.
        expect(talker.summary.identity.trusted, isFalse);
        expect(talker.summary.completion.mechanism, CompletionMechanism.gsParenH);
        expect(talker.summary.completion.gradeCeiling,
            ConfidenceGrade.aJobLevelConfirmation);
        // The printless probe promotes the flag and not its provenance.
        expect(talker.summary.completion.provenance, Provenance.unverified);
        expect(talker.summary.degradations.any((l) => l.contains('empty buffer')),
            isTrue);
        expect(talker.dleEotHex, isNotEmpty);

        final quiet = found.firstWhere((p) => p.status == DetectionStatus.silent);
        expect(quiet.portOpen, isTrue);
        expect(quiet.dleEotHex, isEmpty);

        final dead = found.firstWhere((p) => p.status == DetectionStatus.unreachable);
        expect(dead.portOpen, isFalse);
        expect(dead.summary.profileId, isEmpty);

        // The whole point: not one printable byte reached either live device.
        expect(answering.printDataBytes, 0);
        expect(silent.printDataBytes, 0);

        // And the stream form sees the same candidates.
        final streamed = await driver
            .autoDetectStream(
              endpoints: [answering.endpoint],
              connectTimeout: const Duration(milliseconds: 500),
              responseTimeout: const Duration(milliseconds: 150),
            )
            .toList();
        expect(streamed, hasLength(1));
        expect(streamed.single.status, DetectionStatus.answered);
      } finally {
        answering.stop();
        silent.stop();
        answering.destroy();
        silent.destroy();
        gone.destroy();
      }
    }, skip: skip);
  });

  group('discovery', () {
    late PrinterDriver driver;

    setUp(() => driver = openTestDriver());
    tearDown(() => driver.dispose());

    test('sweeps a loopback address and writes only DLE EOT 1', () {
      final answering = ScriptedListener.start('ok');
      try {
        final found = driver.discover(
          subnetCidr: '127.0.0.1/32',
          port: answering.port,
          connectTimeout: const Duration(milliseconds: 500),
          responseTimeout: const Duration(milliseconds: 300),
        );
        expect(found, hasLength(1));
        expect(found.single.ip, '127.0.0.1');
        expect(found.single.portOpen, isTrue);
        expect(found.single.answered, isTrue);
        // The scripted device's DLE EOT 1 answer: online, drawer pin high.
        expect(found.single.dleEotHex, '16');
        expect(answering.printDataBytes, 0);

        // A CIDR wider than /16 is a mistyped subnet, not a venue, and is refused.
        expect(
          () => driver.discover(subnetCidr: '10.0.0.0/8'),
          throwsA(isA<PrinterDriverException>()),
        );
      } finally {
        answering.stop();
        answering.destroy();
      }
    }, skip: skip);

    test('the detection enums mirror the ABI spellings', () {
      final bindings = testingBindings;
      expect(ProfileSelection.nativeCount,
          bindings.testCppEnumCount(PdTestEnum.profileSelection.nativeValue));
      expect(DetectionStatus.nativeCount,
          bindings.testCppEnumCount(PdTestEnum.detectionStatus.nativeValue));
      for (final member in ProfileSelection.values) {
        if (member == ProfileSelection.unrecognized) continue;
        expect(
          readNativeString(bindings.profileSelectionName(member.nativeValue)),
          readNativeString(bindings.testCppEnumName(
              PdTestEnum.profileSelection.nativeValue, member.nativeValue)),
        );
      }
      for (final member in DetectionStatus.values) {
        if (member == DetectionStatus.unrecognized) continue;
        expect(
          readNativeString(bindings.detectionStatusName(member.nativeValue)),
          readNativeString(bindings.testCppEnumName(
              PdTestEnum.detectionStatus.nativeValue, member.nativeValue)),
        );
      }
    }, skip: skip);
  });
}
