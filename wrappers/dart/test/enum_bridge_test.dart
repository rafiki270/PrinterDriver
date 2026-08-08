/// The mirroring guarantee, checked one layer further out than the C suite checks it.
///
/// `capi/tests/test_capi.c` proves pd.h and the C++ core agree. This proves the Dart
/// enums agree with both — member count, member value and, where the core has a
/// spelling, member name — by asking the enum bridge of `capi/tests/pd_test_support.h`
/// what the core says rather than by trusting a comment. A member added to the core
/// therefore fails here instead of reaching an app as a silent hole.
library;

import 'dart:ffi';

import 'package:printerdriver/printerdriver.dart';
import 'package:printerdriver/src/allocation.dart';
import 'package:printerdriver/src/bindings.dart';
import 'package:test/test.dart';

import 'support/native_library.dart';

/// One mirrored enum: what Dart says, and how to ask the native side the same thing.
final class _Mirror {
  const _Mirror(
    this.bridge,
    this.dartCount,
    this.dartValues, {
    this.expectedNames,
  });

  final PdTestEnum bridge;

  /// The `nativeCount` constant the Dart enum publishes.
  final int dartCount;

  /// Native values in Dart declaration order, excluding `unrecognized`.
  final List<int> dartValues;

  /// The spelling the core is expected to use, derived from the Dart member names, or
  /// null for the enums the core gives no `to_string`.
  final List<String>? expectedNames;
}

/// `Queued` from `queued`: the Dart spelling of a mirrored member is the core's with a
/// lower-case first letter, so deriving one from the other is a real comparison.
List<String> _coreSpelling(Iterable<String> dartNames) => dartNames
    .map((name) => name[0].toUpperCase() + name.substring(1))
    .toList(growable: false);

void main() {
  final skip = skipReasonWhenLibraryMissing;

  group('enum bridge', () {
    late PrinterDriverBindings bindings;
    late List<_Mirror> mirrors;
    late Map<PdTestEnum, Pointer<Char> Function(int)> namers;

    setUpAll(() {
      bindings = testingBindings;

      final states = JobState.values.where((v) => v != JobState.unrecognized);
      final confidences = ConfidenceLevel.values
          .where((v) => v != ConfidenceLevel.unrecognized);
      final deviceEvents =
          DeviceEvent.values.where((v) => v != DeviceEvent.unrecognized);
      final reasons =
          FailureReason.values.where((v) => v != FailureReason.unrecognized);
      final cuts = CutSetting.values.where((v) => v != CutSetting.unrecognized);
      final preflights =
          PreflightMode.values.where((v) => v != PreflightMode.unrecognized);
      final kinds =
          PayloadKind.values.where((v) => v != PayloadKind.unrecognized);
      final completions = CompletionMechanism.values
          .where((v) => v != CompletionMechanism.unrecognized);
      final variants =
          CutVariant.values.where((v) => v != CutVariant.unrecognized);
      final alignments =
          Alignment.values.where((v) => v != Alignment.unrecognized);
      final codePages =
          CodePage.values.where((v) => v != CodePage.unrecognized);
      final binarizations =
          Binarization.values.where((v) => v != Binarization.unrecognized);

      mirrors = <_Mirror>[
        _Mirror(PdTestEnum.jobState, JobState.nativeCount,
            states.map((v) => v.nativeValue).toList(),
            expectedNames: _coreSpelling(states.map((v) => v.name))),
        _Mirror(PdTestEnum.confidence, ConfidenceLevel.nativeCount,
            confidences.map((v) => v.nativeValue).toList(),
            expectedNames: _coreSpelling(confidences.map((v) => v.name))),
        _Mirror(PdTestEnum.deviceEvent, DeviceEvent.nativeCount,
            deviceEvents.map((v) => v.nativeValue).toList(),
            expectedNames: _coreSpelling(deviceEvents.map((v) => v.name))),
        _Mirror(PdTestEnum.failureReason, FailureReason.nativeCount,
            reasons.map((v) => v.nativeValue).toList(),
            expectedNames: _coreSpelling(reasons.map((v) => v.name))),
        // The outcome enum has no Dart enum at all: it is the sealed JobResult, whose
        // three cases are checked against the ABI in print_job_test.dart.
        const _Mirror(PdTestEnum.jobOutcome, 3, <int>[0, 1, 2],
            expectedNames: <String>['Done', 'Failed', 'Unknown']),
        _Mirror(PdTestEnum.cut, CutSetting.nativeCount,
            cuts.map((v) => v.nativeValue).toList()),
        _Mirror(PdTestEnum.preflight, PreflightMode.nativeCount,
            preflights.map((v) => v.nativeValue).toList()),
        // Named `Raster` by the core and `rasterRgba8` here, after the C constant, which
        // is why this one spells its expectation out rather than deriving it.
        _Mirror(PdTestEnum.payloadKind, PayloadKind.nativeCount,
            kinds.map((v) => v.nativeValue).toList(),
            expectedNames: const <String>['Raster', 'Document', 'Raw']),
        _Mirror(PdTestEnum.completion, CompletionMechanism.nativeCount,
            completions.map((v) => v.nativeValue).toList(),
            expectedNames: _coreSpelling(completions.map((v) => v.name))),
        _Mirror(PdTestEnum.cutVariant, CutVariant.nativeCount,
            variants.map((v) => v.nativeValue).toList(),
            expectedNames: _coreSpelling(variants.map((v) => v.name))),
        _Mirror(PdTestEnum.alignment, Alignment.nativeCount,
            alignments.map((v) => v.nativeValue).toList()),
        _Mirror(PdTestEnum.codePage, CodePage.nativeCount,
            codePages.map((v) => v.nativeValue).toList()),
        _Mirror(PdTestEnum.binarization, Binarization.nativeCount,
            binarizations.map((v) => v.nativeValue).toList()),
      ];

      namers = <PdTestEnum, Pointer<Char> Function(int)>{
        PdTestEnum.jobState: bindings.jobStateName,
        PdTestEnum.confidence: bindings.confidenceLevelName,
        PdTestEnum.deviceEvent: bindings.deviceEventName,
        PdTestEnum.failureReason: bindings.failureReasonName,
        PdTestEnum.jobOutcome: bindings.jobOutcomeName,
        PdTestEnum.payloadKind: bindings.payloadKindName,
        PdTestEnum.completion: bindings.completionMechanismName,
        PdTestEnum.cutVariant: bindings.cutVariantName,
      };
    });

    test('every mirrored enum has the member count the C++ core reports', () {
      expect(mirrors, hasLength(PdTestEnum.values.length));
      for (final mirror in mirrors) {
        final label =
            readNativeString(bindings.testEnumLabel(mirror.bridge.nativeValue));
        expect(
          bindings.testCppEnumCount(mirror.bridge.nativeValue),
          mirror.dartCount,
          reason: 'member count drifted for $label',
        );
        expect(mirror.dartValues, hasLength(mirror.dartCount),
            reason:
                'the Dart enum for $label declares a different number of members '
                'than its own nativeCount');
      }
      // The sentinel is not an enum id, which is what makes the count above complete.
      expect(bindings.testCppEnumCount(PdTestEnum.total), -1);
    });

    test('every member carries the value the core gives it at that index', () {
      for (final mirror in mirrors) {
        final label =
            readNativeString(bindings.testEnumLabel(mirror.bridge.nativeValue));
        for (var index = 0; index < mirror.dartCount; index++) {
          expect(
            mirror.dartValues[index],
            bindings.testCppEnumValue(mirror.bridge.nativeValue, index),
            reason: '$label member $index has a different value in Dart',
          );
        }
      }
    });

    test('the named enums are spelled exactly as the core spells them', () {
      for (final mirror in mirrors) {
        final names = mirror.expectedNames;
        final namer = namers[mirror.bridge];
        if (names == null) continue;
        expect(namer, isNotNull,
            reason: '${mirror.bridge.name} has no pd_*_name');
        for (var index = 0; index < mirror.dartCount; index++) {
          expect(readNativeString(namer!(index)), names[index]);
          expect(
            readNativeString(
              bindings.testCppEnumName(mirror.bridge.nativeValue, index),
            ),
            names[index],
            reason:
                'the C++ core spells ${mirror.bridge.name} member $index otherwise',
          );
        }
      }
    });

    test('the enums the core gives no spelling are not given one here either',
        () {
      for (final mirror in mirrors.where((m) => m.expectedNames == null)) {
        expect(
          bindings.testCppEnumName(mirror.bridge.nativeValue, 0),
          nullptr,
          reason: '${mirror.bridge.name} gained a to_string in the core',
        );
      }
    });

    test(
        'code pages are the ESC t n operands, iterated through pd_code_page_at',
        () {
      final fromAbi = <int>[
        for (var index = 0; index < CodePage.nativeCount; index++)
          bindings.codePageAt(index),
      ];
      expect(fromAbi, <int>[0, 2, 16, 18, 19]);
      expect(
        CodePage.values.where((page) => page != CodePage.unrecognized).map(
              (page) => page.nativeValue,
            ),
        fromAbi,
        reason:
            'the code page enum is not contiguous; nativeCount is a member count',
      );
      // Out of range is not a member: pd.h answers PC437 rather than a stray value.
      expect(bindings.codePageAt(CodePage.nativeCount),
          CodePage.pc437.nativeValue);
    });

    test(
        'a value this build does not know becomes unrecognized, never a member',
        () {
      expect(JobState.fromNative(JobState.nativeCount), JobState.unrecognized);
      expect(ConfidenceLevel.fromNative(99), ConfidenceLevel.unrecognized);
      expect(DeviceEvent.fromNative(DeviceEvent.nativeCount),
          DeviceEvent.unrecognized);
      expect(FailureReason.fromNative(-7), FailureReason.unrecognized);
      expect(CodePage.fromNative(1), CodePage.unrecognized,
          reason: '1 is a gap in the ESC t n operands, not a member');
      expect(
        CompletionMechanism.fromNative(CompletionMechanism.nativeCount),
        CompletionMechanism.unrecognized,
      );

      // And it cannot be handed back to the ABI as if it were a real member.
      final arena = Arena();
      addTearDown(arena.releaseAll);
      final options = arena.allocate<PdJobOptions>(sizeOf<PdJobOptions>());
      expect(
        () => const JobOptions(cut: CutSetting.unrecognized)
            .fillNative(arena, options),
        throwsArgumentError,
      );
    });
  }, skip: skip);
}
