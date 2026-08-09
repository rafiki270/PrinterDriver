/// M19 — the receipt DSL through the Dart surface (docs/receipt-dsl.md).
///
/// End to end over the real engine, like every other suite here: the template is parsed,
/// bound and rendered by the C++ core, and the bytes are the bytes the scripted device
/// received. Nothing about the DSL is re-implemented in Dart, so what these prove is that
/// the Dart surface reaches the real one.
library;

import 'package:printerdriver/printerdriver.dart';
import 'package:test/test.dart';

import 'support/native_library.dart';

/// A template with an `each` loop and the built-in `upper` formatter, plus a `meta` the
/// job has to honour.
const String orderTemplate = '''
{ "v": 1, "template": true,
  "meta": { "cut": "full", "margins": { "topDots": 24 } },
  "blocks": [
    { "text": "{{venue.name|upper}}" },
    { "each": "order.items",
      "block": { "text": "{{qty}}x {{name|upper}}" } } ] }
''';

const Map<String, Object?> orderModel = <String, Object?>{
  'venue': <String, Object?>{'name': 'my restaurant'},
  'order': <String, Object?>{
    'items': <Object?>[
      <String, Object?>{'qty': 2, 'name': 'pilsner'},
      <String, Object?>{'qty': 1, 'name': 'goulash'},
    ],
  },
};

void main() {
  final skip = skipReasonWhenLibraryMissing;

  group('receipt DSL', () {
    late PrinterDriver driver;

    setUp(() => driver = openTestDriver());
    tearDown(() => driver.dispose());

    test('printDocument binds a template and prints it through the engine',
        () async {
      final printer = driver.addScriptedPrinterForTesting(
        script: 'ok',
        printerId: 'dart-doc',
      );
      final device = printer.scriptedDeviceForTesting!;

      final job = printer.printDocument(
        orderTemplate,
        model: orderModel,
        options: const JobOptions(key: 'order-7F3A'),
      );
      expect(job.renderReport, isEmpty);
      expect(job.key, 'order-7F3A');

      // A template job is an ordinary job: it earns exactly what the fence earns.
      final result = await job.result;
      expect(result, isA<JobDone>());
      expect((result as JobDone).confidence, ConfidenceLevel.cutFaultFree);
      expect(result.grade, ConfidenceGrade.aJobLevelConfirmation);
      expect(result.method, 'GS(H) fn48');

      // The formatter ran, the loop repeated in model order, and no placeholder survived.
      expect(device.received('MY RESTAURANT'), isTrue);
      expect(device.received('2x PILSNER'), isTrue);
      expect(device.received('1x GOULASH'), isTrue);
      expect(device.received('{{'), isFalse);
      expect(device.cuts, 1);

      // Rule 2 of the idempotency contract reaches this entry point too.
      final again = printer.printDocument(
        orderTemplate,
        model: orderModel,
        options: const JobOptions(key: 'order-7F3A'),
      );
      expect(identical(again, job), isTrue);
      expect(device.cuts, 1);
    });

    test('renderDocument declares a degradation and prints nothing', () {
      final printer = driver.addScriptedPrinterForTesting(
        script: 'no-barcode',
        printerId: 'dart-no-gs-k',
      );
      final device = printer.scriptedDeviceForTesting!;

      final rendered = printer.renderDocument(<String, Object?>{
        'v': 1,
        'blocks': <Object?>[
          <String, Object?>{'text': 'WIDGET CO'},
          <String, Object?>{'barcode': '12345670', 'symbology': 'code128'},
        ],
      });

      // The text still rendered: a declared degradation is not a failure.
      expect(rendered.bytes, isNotEmpty);
      expect(rendered.report, hasLength(1));

      final entry = rendered.report.single;
      expect(entry.kind, ReportKind.unsupportedBlock);
      expect(entry.block, 'blocks[1]');
      expect(entry.requested, contains('code128'));
      expect(entry.delivered, 'omitted');
      expect(entry.path, RenderPath.notRendered);
      expect(entry.note, isNotEmpty);

      // Rendering is not printing.
      expect(device.printDataBytes, 0);
      expect(device.cuts, 0);
    });

    test('renderDocument returns the document own meta', () {
      final printer = driver.addScriptedPrinterForTesting(
        script: 'ok',
        printerId: 'dart-meta',
      );
      final rendered =
          printer.renderDocument(orderTemplate, model: orderModel);
      expect(rendered.meta.cut, CutSetting.full);
      expect(rendered.meta.topFeedDots, 24);
      expect(rendered.meta.bottomFeedDots, 0);
      expect(rendered.report, isEmpty);
      expect(rendered.codePage, isA<CodePage>());
    });

    test('malformed documents are refused and nothing is printed', () {
      final printer = driver.addScriptedPrinterForTesting(
        script: 'ok',
        printerId: 'dart-doc-bad',
      );
      final device = printer.scriptedDeviceForTesting!;

      expect(() => printer.renderDocument('this is not json'),
          throwsA(isA<PrinterDriverException>()));
      // A template with no model is refused rather than printed: a receipt full of
      // {{order.total}} is worse than no receipt, because it looks like one.
      expect(() => printer.renderDocument(orderTemplate),
          throwsA(isA<PrinterDriverException>()));
      expect(() => printer.printDocument('this is not json'),
          throwsA(isA<PrinterDriverException>()));

      expect(device.printDataBytes, 0);
      expect(device.cuts, 0);
    });

    // docs/api.md §17.1: registerFormatter was accepted and stored, and nothing a
    // wrapper could call consulted it. This is that call site.
    test('a registered formatter fires through the render path', () async {
      const template =
          '{"v":1,"template":true,"blocks":[{"text":"{{item|acme.stars}}"}]}';
      const model = <String, Object?>{'item': 'tip'};

      final control = driver.addScriptedPrinterForTesting(
        script: 'ok',
        printerId: 'dart-fmt-control',
      );
      expect(
        control.renderDocument(template, model: model).report.single.kind,
        ReportKind.unknownFormatter,
      );

      // `dart:ffi` cannot serve a callback the core invokes on its own render thread, so
      // the Dart surface takes native function pointers (docs/api.md §16). The reference
      // formatter lives in the testing library next to the scripted device.
      driver.registerFormatter(CustomFormatter.fromLibrary(
        testingLibrary,
        name: 'acme.stars',
        formatter: 'pd_test_stars_formatter',
      ));

      final printer = driver.addScriptedPrinterForTesting(
        script: 'ok',
        printerId: 'dart-fmt',
      );
      final device = printer.scriptedDeviceForTesting!;

      final rendered = printer.renderDocument(template, model: model);
      expect(rendered.report, isEmpty);

      // And on paper, not only in a preview.
      final job = printer.printDocument(template, model: model);
      expect(await job.result, isA<JobDone>());
      expect(device.received('***tip***'), isTrue);
    });
  }, skip: skip);
}
