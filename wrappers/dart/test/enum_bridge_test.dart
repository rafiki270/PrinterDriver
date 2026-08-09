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
      final grades = ConfidenceGrade.values
          .where((v) => v != ConfidenceGrade.unrecognized);
      final authorities = CompletionAuthority.values
          .where((v) => v != CompletionAuthority.unrecognized);
      // M14 — docs/cash-drawer.md.
      final drawerStates =
          DrawerState.values.where((v) => v != DrawerState.unrecognized);
      final drawerPorts = DrawerPortStandard.values
          .where((v) => v != DrawerPortStandard.unrecognized);
      final drawerKicks = DrawerKickMethod.values
          .where((v) => v != DrawerKickMethod.unrecognized);
      final drawerStatuses = DrawerStatusMethod.values
          .where((v) => v != DrawerStatusMethod.unrecognized);

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
        // The core spells these APlus_DurableQueryableJob..E_TransportOnly, which the
        // Dart member names carry as aPlusDurableQueryableJob..eTransportOnly.
        _Mirror(PdTestEnum.confidenceGrade, ConfidenceGrade.nativeCount,
            grades.map((v) => v.nativeValue).toList(),
            expectedNames: const <String>[
              'APlus_DurableQueryableJob',
              'A_JobLevelConfirmation',
              'B_OrderedDeviceResponse',
              'C_DeviceStatusAround',
              'D_SpoolerCompleted',
              'E_TransportOnly',
            ]),
        _Mirror(PdTestEnum.completionAuthority, CompletionAuthority.nativeCount,
            authorities.map((v) => v.nativeValue).toList(),
            expectedNames: _coreSpelling(authorities.map((v) => v.name))),
        // M14. The Dart names are the core's with a lower-case first letter everywhere
        // except the two the C constants forced — Epson24V6P6C and GsR2 — which is why
        // those two spell their expectation out.
        _Mirror(PdTestEnum.drawerState, DrawerState.nativeCount,
            drawerStates.map((v) => v.nativeValue).toList(),
            expectedNames: _coreSpelling(drawerStates.map((v) => v.name))),
        _Mirror(PdTestEnum.drawerPortStandard, DrawerPortStandard.nativeCount,
            drawerPorts.map((v) => v.nativeValue).toList(),
            expectedNames: const <String>[
              'Epson24V6P6C',
              'Star24V6P6C',
              'Generic12V6P6C',
              'Unknown',
            ]),
        _Mirror(PdTestEnum.drawerKickMethod, DrawerKickMethod.nativeCount,
            drawerKicks.map((v) => v.nativeValue).toList(),
            expectedNames: _coreSpelling(drawerKicks.map((v) => v.name))),
        _Mirror(PdTestEnum.drawerStatusMethod, DrawerStatusMethod.nativeCount,
            drawerStatuses.map((v) => v.nativeValue).toList(),
            expectedNames: const <String>[
              'GsR2',
              'Asb',
              'StarSignal',
              'VendorSdk',
              'None',
            ]),
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
        PdTestEnum.confidenceGrade: bindings.confidenceGradeName,
        PdTestEnum.completionAuthority: bindings.completionAuthorityName,
        PdTestEnum.drawerState: bindings.drawerStateName,
        PdTestEnum.drawerPortStandard: bindings.drawerPortStandardName,
        PdTestEnum.drawerKickMethod: bindings.drawerKickMethodName,
        PdTestEnum.drawerStatusMethod: bindings.drawerStatusMethodName,
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
      expect(ConfidenceGrade.fromNative(ConfidenceGrade.nativeCount),
          ConfidenceGrade.unrecognized);
      expect(ConfidenceGrade.unrecognized.letter, '?');
      expect(
          CompletionAuthority.fromNative(-1), CompletionAuthority.unrecognized);
      expect(Provenance.fromNative(Provenance.nativeCount),
          Provenance.unrecognized);
      expect(CommandLanguage.fromNative(CommandLanguage.nativeCount),
          CommandLanguage.unrecognized);
      // M14. Every drawer substitution has to be the one that claims least: an
      // unrecognized state cannot become a verified open, and an unrecognized port
      // cannot become a fireable one.
      expect(DrawerState.fromNative(DrawerState.nativeCount),
          DrawerState.unrecognized);
      expect(DrawerPortStandard.fromNative(-9),
          DrawerPortStandard.unrecognized);
      expect(DrawerKickMethod.fromNative(DrawerKickMethod.nativeCount),
          DrawerKickMethod.unrecognized);
      expect(DrawerStatusMethod.fromNative(DrawerStatusMethod.nativeCount),
          DrawerStatusMethod.unrecognized);

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

    test('the grade hierarchy is strongest-first, A+ included', () {
      // docs/compatibility-brief.md §24. A+ took value 0 and pushed A..E up by one, so
      // the enum still reads strongest-first and two grades compare numerically. A
      // mirror that kept the old numbering would report every grade one rung too strong
      // — the single most damaging way this table could be wrong.
      expect(ConfidenceGrade.aPlusDurableQueryableJob.nativeValue, 0);
      expect(ConfidenceGrade.aJobLevelConfirmation.nativeValue, 1);
      expect(ConfidenceGrade.eTransportOnly.nativeValue, 5);
      expect(ConfidenceGrade.nativeCount, 6);

      final grades = ConfidenceGrade.values
          .where((v) => v != ConfidenceGrade.unrecognized);
      expect(
          grades.map((v) => v.letter), <String>['A+', 'A', 'B', 'C', 'D', 'E']);
      for (final grade in grades) {
        expect(
          readNativeString(bindings.confidenceGradeLetter(grade.nativeValue)),
          grade.letter,
          reason: 'pd_confidence_grade_letter disagrees for ${grade.name}',
        );
      }
      // Out of range is not a member: the ABI answers "" where Dart answers "?", and
      // neither invents a letter.
      expect(
        readNativeString(
            bindings.confidenceGradeLetter(ConfidenceGrade.nativeCount)),
        isEmpty,
      );
    });

    test('nothing in this build can produce the A+ grade', () {
      // The ePOS transport does not exist in this core, so no pd_job_result can carry
      // A+. It is mirrored anyway because these enums are closed and four wrappers
      // mirror them: adding the member later would renumber every mirror a second time.
      // A profile that reports an ePOS JobID is describing hardware, not making a
      // claim, and grades A — which is why eposJobId is a completion *mechanism* here
      // and never a grade.
      expect(CompletionMechanism.eposJobId.nativeValue, 3);
      expect(ConfidenceGrade.aPlusDurableQueryableJob.letter, 'A+');
    });

    test('provenance and command language mirror their pd_*_name spellings',
        () {
      // Neither has an id in pd_test_support.h's enum bridge, so the comparison is
      // against the ABI's own naming functions rather than against the C++ core
      // directly. That still catches the failure that matters here — a member added,
      // reordered or renamed on the C side — because pd.h and the core are already
      // static_asserted against each other.
      final provenances =
          Provenance.values.where((v) => v != Provenance.unrecognized);
      expect(provenances.map((v) => v.nativeValue), <int>[0, 1, 2]);
      expect(provenances, hasLength(Provenance.nativeCount));

      final expectedProvenance = _coreSpelling(provenances.map((v) => v.name));
      for (var index = 0; index < Provenance.nativeCount; index++) {
        expect(readNativeString(bindings.provenanceName(index)),
            expectedProvenance[index]);
      }
      expect(readNativeString(bindings.provenanceName(Provenance.nativeCount)),
          isEmpty);

      final languages = CommandLanguage.values
          .where((v) => v != CommandLanguage.unrecognized);
      expect(
          languages.map((v) => v.nativeValue), <int>[0, 1, 2, 3, 4, 5, 6, 7]);
      expect(languages, hasLength(CommandLanguage.nativeCount));

      final expectedLanguage = _coreSpelling(languages.map((v) => v.name));
      for (var index = 0; index < CommandLanguage.nativeCount; index++) {
        expect(readNativeString(bindings.commandLanguageName(index)),
            expectedLanguage[index]);
      }
      // The two that are one letter apart in Dart and a whole ecosystem apart on paper:
      // Epson ESC/POS is the only implemented language, Brother ESC/P is refused.
      expect(readNativeString(bindings.commandLanguageName(0)), 'EscPos');
      expect(readNativeString(bindings.commandLanguageName(7)), 'EscP');
      expect(
        readNativeString(
            bindings.commandLanguageName(CommandLanguage.nativeCount)),
        isEmpty,
      );
    });
  }, skip: skip);
}
