/// Prints a receipt on a network printer and reports what actually happened.
///
/// ```sh
/// dart run example/main.dart 192.168.1.101
/// ```
///
/// Needs the native library: build it with
/// `cmake -S . -B build-dart -DPD_BUILD_SHARED_CAPI=ON && cmake --build build-dart`
/// from the repository root, then either run from a directory the loader searches or
/// set `PRINTERDRIVER_LIB_PATH` to the built `libprinterdriver_capi.dylib`/`.so`.
library;

import 'dart:io';

import 'package:printerdriver/printerdriver.dart';

Future<void> main(List<String> arguments) async {
  final host = arguments.isEmpty ? '192.168.1.101' : arguments.first;
  final orderId = arguments.length > 1 ? arguments[1] : 'demo-7F3A-92C1';

  final driver = PrinterDriver.open(
    // Where the job journal lives. Without it there is no crash recovery: a job that
    // was in flight when the process died cannot be asked about afterwards.
    storageDirectory: '${Directory.systemTemp.path}/printerdriver-example',
    onLog: (message) => stderr.writeln('[printerdriver] $message'),
  );

  try {
    final kitchen = driver.addTcpPrinter(
      host: host,
      printerId: 'kitchen-1',
      widthDots: 576,
    );

    // Live, from the moment it is listened to: this is what replaces polling a printer
    // every few seconds to ask whether it is still there.
    final deviceEvents = kitchen.events.listen(
      (event) => stdout.writeln('printer: ${event.name}'),
    );

    final receipt = Payload.document(
      <DocumentOp>[
        DocumentOp.align(Alignment.center),
        DocumentOp.bold(true),
        const DocumentOp.line('THE CORNER CAFE'),
        const DocumentOp.bold(false),
        DocumentOp.align(Alignment.left),
        const DocumentOp.line(),
        DocumentOp.line('ORDER $orderId'),
        const DocumentOp.line('--------------------------------'),
        const DocumentOp.line('1x Flat white              3.40'),
        const DocumentOp.line('2x Croissant               5.00'),
        const DocumentOp.line('--------------------------------'),
        const DocumentOp.line('TOTAL                      8.40'),
        DocumentOp.feed(3),
      ],
      codePage: CodePage.pc437,
    );

    // The closure form (docs/api.md §12): onProgress sees every job event in order,
    // and the future carries the terminal answer exactly once. `key` is what makes a
    // resubmission of the same ticket return the existing job instead of printing it
    // twice, so it is derived from the order, never generated.
    final result = await kitchen.send(
      receipt,
      key: '$orderId#kitchen-1',
      options: const JobOptions(cut: CutSetting.partial),
      onProgress: (event) =>
          stdout.writeln('  ${event.state.name} (${event.confidence.name})'),
    );

    // Three cases, no boolean. The compiler will not let this switch forget one.
    switch (result) {
      case JobDone(:final confidence):
        stdout.writeln('printed, backed by ${confidence.name}');
      case JobFailed(:final reason):
        // Nothing printed, or the failure is confirmed: resubmitting the same key is
        // safe.
        stderr.writeln('failed: ${reason.name}');
        exitCode = 1;
      case JobUnknown(:final reason):
        // Bytes went out and nothing came back. NOT a failure. Ask a human whether
        // paper came out, then either accept it or call forceReprint.
        stderr.writeln(
            'unknown (${reason.name}) — check the printer before reprinting');
        exitCode = 2;
    }

    await deviceEvents.cancel();
  } on PrinterDriverException catch (error) {
    stderr.writeln(error.message);
    exitCode = 1;
  } finally {
    driver.dispose();
  }
}
