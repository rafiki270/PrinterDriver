/// The platform-owned link, driven from Dart (docs/compatibility-brief.md §25).
///
/// `capi/tests/test_capi.c` proves this at the C level with a vtable written in C. This
/// proves it at the level a Flutter app actually works at: the vtable is assembled in
/// Dart, registered through [PrinterDriver.addCustomPrinter], and the far side is the
/// scripted printer of `capi/tests/pd_test_support.h`, whose reader thread answers from
/// a thread that is not the one `write` was called on — as pd.h requires and as every
/// real Bluetooth stack behaves.
///
/// What has to hold is not that bytes move. It is that a printer reached over a link the
/// core knows nothing about gets the same ordered fence and reports the same grade,
/// authority and method a TCP printer reports, because the moment a transport could
/// weaken the completion story every wrapper would become part of it.
library;

import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:typed_data';

import 'package:printerdriver/printerdriver.dart';
import 'package:test/test.dart';

import 'support/native_library.dart';

Payload rawText(String text) =>
    Payload.raw(Uint8List.fromList(utf8.encode(text)));

/// A driver and a scripted link, torn down in the order pd.h requires: the driver
/// first, because until `pd_destroy` returns the core may still be writing to the link.
({PrinterDriver driver, ScriptedLink link}) openLinkedDriver({
  String script = 'ok',
}) {
  final driver = openTestDriver();
  final link = driver.scriptedLinkForTesting(script: script);
  addTearDown(() {
    driver.dispose();
    link.destroy();
  });
  return (driver: driver, link: link);
}

/// The vtable a Bluetooth wrapper would build, pointed at [link].
///
/// The scripted link's three entry points already have the signatures
/// `pd_transport_vtable` wants, so this is the real registration path and not a
/// test-only shortcut: an application resolves its own plugin's symbols the same way.
CustomTransport transportFor(ScriptedLink link, {String? description}) =>
    CustomTransport.fromLibrary(
      testingLibrary,
      connect: ScriptedLink.connectSymbol,
      write: ScriptedLink.writeSymbol,
      close: ScriptedLink.closeSymbol,
      ctx: link.ctx,
      description: description,
    );

void main() {
  final skip = skipReasonWhenLibraryMissing;

  group('custom transport', () {
    test('a caller-owned link earns exactly the claim a TCP printer earns',
        () async {
      final (:driver, :link) = openLinkedDriver();
      final printer = driver.addCustomPrinter(
        transportFor(link, description: 'bt-spp:00:11:22:33:44:55'),
        profileId: 'xp-s260m',
        widthDots: 576,
      );
      link.bindTo(printer);

      // The id derives from the vtable's description, so two Bluetooth printers are
      // distinguishable without the caller inventing names for them.
      expect(printer.id, contains('bt-spp'));
      expect(printer.widthDots, 576);
      expect(printer.completion, CompletionMechanism.gsParenH);

      final result = await printer.send(
        rawText('BLUETOOTH TICKET'),
        key: 'dart-bt-1',
      );

      expect(result, isA<JobDone>());
      // Not weaker for being Bluetooth, and not stronger either: the identical four
      // facts `print_job_test.dart` asserts for the scripted TCP-style device.
      expect(result.confidence, ConfidenceLevel.cutFaultFree);
      expect(result.grade, ConfidenceGrade.aJobLevelConfirmation);
      expect(result.grade.letter, 'A');
      expect(result.authority, CompletionAuthority.physicalPrinter);
      expect(result.method, 'GS(H) fn48');

      expect(link.connects, greaterThanOrEqualTo(1));
      expect(link.bytesWritten, greaterThan(0));
      expect(link.cuts, 1);
      expect(link.received('BLUETOOTH TICKET'), isTrue);
    });

    test('the fence survives the link: a queued-fence profile still caps',
        () async {
      final (:driver, :link) = openLinkedDriver(script: 'gsr1');
      final printer = driver.addCustomPrinter(transportFor(link));
      link.bindTo(printer);

      final result = await printer.send(rawText('ORDERED FENCE ONLY'));

      // The transport did not change what the device can prove, which is the whole
      // claim this file exists to make.
      expect(result, isA<JobDone>());
      expect(result.confidence, ConfidenceLevel.cutProcessed);
      expect(printer.id, contains('custom'),
          reason: 'an empty description is "custom", per pd.h');
    });

    test('a link that refuses to open fails the job and writes nothing',
        () async {
      final (:driver, :link) = openLinkedDriver();
      link.refuseConnections();
      final printer = driver.addCustomPrinter(transportFor(link));
      link.bindTo(printer);

      final result = await printer.send(rawText('OUT OF RANGE'));

      expect(result, isA<JobFailed>());
      expect((result as JobFailed).reason, FailureReason.transportUnreachable);
      expect(link.connects, greaterThanOrEqualTo(1));
      expect(link.bytesWritten, 0);
    });

    test('feedBytes reports honestly whether anything was listening', () async {
      final (:driver, :link) = openLinkedDriver();
      final printer = driver.addCustomPrinter(transportFor(link));
      link.bindTo(printer);

      // Nothing is dialled until a job needs the link, so there is no reader yet. That
      // is information rather than an error — the same answer a socket nobody reads
      // gives — and it is why this returns a bool instead of throwing.
      expect(printer.feedBytes(const <int>[0x12]), isFalse);

      expect(await printer.send(rawText('RX SIDE')), isA<JobDone>());

      // The connection outlives the job, which is what lets a printer report a cover
      // opening between receipts, so the parser is listening now.
      expect(printer.feedBytes(const <int>[0x12]), isTrue);
      // Zero bytes is nothing to deliver rather than an empty delivery.
      expect(printer.feedBytes(const <int>[]), isFalse);
    });

    test('a dropped link is reported rather than left to time out', () async {
      final (:driver, :link) = openLinkedDriver(script: 'gsr1');
      final printer = driver.addCustomPrinter(transportFor(link));
      link.bindTo(printer);
      final events = <DeviceEvent>[];
      final subscription = printer.events.listen(events.add);
      addTearDown(subscription.cancel);

      expect(printer.reportLinkDropped('out of range'), isFalse,
          reason: 'there is no live transport to notify before the first job');

      expect(await printer.send(rawText('FIRST')), isA<JobDone>());
      expect(printer.reportLinkDropped('pairing revoked'), isTrue);
      await _until(() => events.contains(DeviceEvent.connectionLost));

      expect(events, contains(DeviceEvent.connectionLost));
    });

    test('the Dart close hook is a listener, and runs on the isolate',
        () async {
      final driver = openTestDriver();
      final link = driver.scriptedLinkForTesting(script: 'ok');
      addTearDown(link.destroy);

      // Touching Dart state from this callback is the point of the assertion: a
      // listener runs its function on the isolate's event loop after the native call
      // returned, so this is ordinary Dart. The same closure behind connect or write —
      // which must answer the core on its own worker thread — has no safe form at all;
      // see CustomTransport.
      final closed = Completer<void>();
      final transport = CustomTransport.withDartClose(
        connect:
            testingLibrary.lookup<NativeFunction<PdTransportConnectNative>>(
                ScriptedLink.connectSymbol),
        write: testingLibrary.lookup<NativeFunction<PdTransportWriteNative>>(
            ScriptedLink.writeSymbol),
        onClose: () {
          if (!closed.isCompleted) closed.complete();
        },
        ctx: link.ctx,
        description: 'bt-spp:dart-close',
      );
      final printer = driver.addCustomPrinter(transport);
      link.bindTo(printer);

      expect(await printer.send(rawText('CLOSE ME')), isA<JobDone>());
      // The core closes a still-connected link exactly once, on the way out of
      // pd_destroy. The scripted link's own close was never registered, so its counter
      // stays at zero and the Dart hook is what ran instead.
      driver.dispose();
      await closed.future.timeout(const Duration(seconds: 5));
      expect(link.closes, 0);
    });

    test('a registration the core cannot honour is refused, never downgraded',
        () async {
      final (:driver, :link) = openLinkedDriver();

      // pd.h needs at least connect and write; a vtable missing them is not a printer
      // that will fail later, it is not a printer.
      expect(
        () => driver.addCustomPrinter(
          CustomTransport(connect: nullptr, write: nullptr),
        ),
        throwsA(isA<PrinterDriverException>()),
      );

      // An unknown profile id is an error rather than a silent fall back to generic: a
      // caller that asked for a TM-T88VI and got the unknown-device profile would be
      // told a weaker completion story than it asked for, with nothing in the result
      // explaining why.
      expect(
        () => driver.addCustomPrinter(transportFor(link),
            profileId: 'epson_tm_t99'),
        throwsA(isA<PrinterDriverException>()),
      );
      expect(driver.lastError, isNotEmpty);

      // And a symbol that is not exported fails where the stack still explains it,
      // rather than as a jump through a null pointer on the core's worker thread.
      expect(
        () => CustomTransport.fromLibrary(
          testingLibrary,
          connect: 'pd_no_such_symbol',
          write: ScriptedLink.writeSymbol,
        ),
        throwsArgumentError,
      );
    });

    test('feeding a printer that owns no link is answered, not fatal', () {
      final driver = openTestDriver();
      addTearDown(driver.dispose);
      final scripted = driver.addScriptedPrinterForTesting(script: 'ok');

      // The scripted printer is not a custom transport, so there is no link to feed.
      // pd.h answers 0 rather than treating it as an error, and so does this.
      expect(scripted.feedBytes(const <int>[0x10, 0x04, 0x01]), isFalse);
      expect(scripted.reportLinkDropped('gone'), isFalse);
    });

    group('a language that is not ESC/POS', () {
      // docs/compatibility-brief.md §16, §17. ZPL, CPCL and Brother Raster are not
      // ESC/POS at any level. The refusal has to happen before the link is opened: a
      // refusal that still wasted a roll would be worse than the bug it replaced.
      const refused = <String, CommandLanguage>{
        'zebra_zq600_plus': CommandLanguage.zpl,
        'brother_rj4000': CommandLanguage.brotherRaster,
      };

      refused.forEach((profileId, language) {
        test('$profileId is refused with no byte handed to write', () async {
          final (:driver, :link) = openLinkedDriver();
          final printer = driver.addCustomPrinter(
            transportFor(link, description: profileId),
            profileId: profileId,
            widthDots: 576,
          );
          link.bindTo(printer);

          expect(printer.language, language);
          expect(printer.completionProvenance, Provenance.unverified);

          final result = await printer.send(
            rawText('LABEL'),
            key: 'dart-$profileId',
          );

          expect(result, isA<JobFailed>());
          expect((result as JobFailed).reason, FailureReason.unsupported);
          // The evidence fields are carried on a failure too, and they say the honest
          // thing: nothing was confirmed by anyone, because nothing was sent.
          expect(result.grade, ConfidenceGrade.eTransportOnly);
          expect(result.grade.letter, 'E');
          expect(result.authority, CompletionAuthority.transportOnly);
          expect(result.method, 'none');

          // Zero bytes, zero connections, zero cuts, zero paper.
          expect(link.bytesWritten, 0);
          expect(link.connects, 0);
          expect(link.cuts, 0);
        });
      });
    });
  }, skip: skip);
}

/// Waits for something the core does on one of its own threads to become visible here.
///
/// Polling rather than a completer because the observable is a native counter or a
/// broadcast stream this test attached to after the fact; the timeout is what turns a
/// contract that stopped holding into a failure instead of a hang.
Future<void> _until(
  bool Function() condition, {
  Duration timeout = const Duration(seconds: 5),
}) async {
  final deadline = DateTime.now().add(timeout);
  while (!condition()) {
    if (DateTime.now().isAfter(deadline)) {
      throw StateError('condition never became true within $timeout');
    }
    await Future<void>.delayed(const Duration(milliseconds: 5));
  }
}
