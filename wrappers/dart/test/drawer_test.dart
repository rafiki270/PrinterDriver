/// M14 — the cash drawer, through the Dart surface (docs/cash-drawer.md).
///
/// The wrapper holds no drawer logic: the sequence, the refusals and the polarity all
/// live in the C++ core. What is under test here is that none of it is lost on the way
/// across — that a refusal is still a refusal, that `kickSentUnverified` does not quietly
/// become a success, and that an uncalibrated switch still reports a level rather than a
/// direction.
library;

import 'package:printerdriver/printerdriver.dart';
import 'package:test/test.dart';

import 'support/native_library.dart';

void main() {
  final skip = skipReasonWhenLibraryMissing;

  group('cash drawer', () {
    late PrinterDriver driver;

    setUp(() => driver = openTestDriver());
    tearDown(() => driver.dispose());

    test('a verified open is reported only when the switch actually moves', () {
      final till = driver.addScriptedPrinterForTesting(
        script: 'drawer',
        printerId: 'till',
      );
      final device = till.scriptedDeviceForTesting!;

      final caps = till.drawerCapabilities;
      expect(caps.present, isTrue);
      expect(caps.kickable, isTrue);
      expect(caps.portStandard, DrawerPortStandard.epson24V6P6C);
      expect(caps.kickMethod, DrawerKickMethod.epsonEscP);
      expect(caps.statusMethod, DrawerStatusMethod.gsR2);
      expect(caps.voltage, 24);
      expect(caps.maxCurrentMa, 1000);
      expect(caps.channelCount, 2);
      // The single fact that separates this arrangement from Star's.
      expect(caps.sensorPin, 3);
      expect(caps.electricalProvenance, Provenance.documented);
      expect(caps.commandsProvenance, Provenance.documented);

      final result = till.openDrawer();
      expect(result.previousState, DrawerState.closed);
      expect(result.state, DrawerState.openVerified);
      expect(result.verified, isTrue);
      expect(result.channel, 1);
      expect(result.pulseMs, 200);
      expect(device.drawerKicks, 1);
      expect(device.drawerIsOpen, isTrue);

      // Step 1 of the sequence: a drawer that is already out is never pulsed again.
      final again = till.openDrawer(pulseMs: 120);
      expect(again.state, DrawerState.open);
      expect(again.pulseMs, 0);
      expect(again.verified, isFalse);
      expect(device.drawerKicks, 1);
    }, skip: skip);

    test('a locked drawer is failedToOpen and not a success', () {
      final till = driver.addScriptedPrinterForTesting(
        script: 'drawer-locked',
        printerId: 'locked',
      );
      final device = till.scriptedDeviceForTesting!;

      final result = till.openDrawer(channel: 2, pulseMs: 120);
      expect(result.previousState, DrawerState.closed);
      expect(result.state, DrawerState.failedToOpen);
      expect(result.verified, isFalse);
      expect(result.channel, 2);
      expect(result.pulseMs, 120);
      // The pulse was real; the drawer was not.
      expect(device.drawerKicks, 1);
      expect(device.drawerIsOpen, isFalse);
    }, skip: skip);

    test('an unclassified port is refused without writing a byte', () {
      final till = driver.addScriptedPrinterForTesting(
        script: 'drawer-unknown-port',
        printerId: 'unclassified',
      );
      final device = till.scriptedDeviceForTesting!;

      expect(till.drawerCapabilities.portStandard, DrawerPortStandard.unknown);
      expect(till.drawerCapabilities.kickable, isFalse);

      final result = till.openDrawer();
      expect(result.state, DrawerState.unknown);
      expect(result.pulseMs, 0);
      // RJ11/RJ12-looking drawer connectors are not a universal electrical standard,
      // and an unclassified port is never energised.
      expect(device.drawerKicks, 0);

      // Reading the switch is still safe on the same hardware: it asks a question and
      // energises nothing, which is what makes it the probe's non-destructive half.
      final reading = till.readDrawerSensor(
        timeout: const Duration(milliseconds: 500),
      );
      expect(reading.available, isTrue);
      expect(reading.answered, isTrue);
      expect(device.drawerKicks, 0);
    }, skip: skip);

    test('an uncalibrated switch reports a level and not a direction', () {
      final till = driver.addScriptedPrinterForTesting(
        script: 'drawer-uncalibrated',
        printerId: 'uncalibrated',
      );
      final device = till.scriptedDeviceForTesting!;
      expect(till.drawerPolarityCalibrated, isFalse);

      // The operator procedure, without the prompts.
      device.drawerIsOpen = false;
      final shut = till.readDrawerSensor(
        timeout: const Duration(milliseconds: 500),
      );
      expect(shut.answered, isTrue);
      expect(shut.needsCalibration, isTrue);
      expect(shut.pinHigh, isFalse);
      // Whether the line reads high or low when the drawer is open depends on the
      // drawer that is plugged in, so until it is measured there is no interpretation.
      expect(shut.state, DrawerState.unknown);

      device.drawerIsOpen = true;
      final open = till.readDrawerSensor(
        timeout: const Duration(milliseconds: 500),
      );
      expect(open.pinHigh, isTrue);
      expect(device.drawerKicks, 0);

      // In-memory driver: the calibration applies to this process and says so by
      // returning false rather than pretending it was persisted.
      expect(
        till.calibrateDrawerPolarity(highMeansOpen: open.pinHigh ?? true),
        isFalse,
      );
      expect(till.drawerPolarityCalibrated, isTrue);
      expect(till.drawerHighMeansOpen, isTrue);

      final after = till.readDrawerSensor(
        timeout: const Duration(milliseconds: 500),
      );
      expect(after.needsCalibration, isFalse);
      expect(after.state, DrawerState.open);
    }, skip: skip);

    test('a label printer has no drawer and fires nothing', () {
      // Zebra speaks ZPL and CPCL, has no drawer port, and every job on it is refused
      // before a byte is written. The drawer call is the same refusal.
      final labels = driver.addTcpPrinter(
        host: '192.0.2.60',
        printerId: 'labels',
        profileId: 'zebra_zq600_plus',
      );

      final caps = labels.drawerCapabilities;
      expect(caps.present, isFalse);
      expect(caps.kickMethod, DrawerKickMethod.unsupported);
      expect(caps.kickable, isFalse);

      final result = labels.openDrawer();
      expect(result.state, DrawerState.unknown);
      expect(result.pulseMs, 0);
    }, skip: skip);
  });
}
