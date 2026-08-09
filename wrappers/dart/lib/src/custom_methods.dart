/// Custom method registration (docs/api.md §16): five ways to extend the SDK at runtime
/// without forking it.
///
/// A vendor idle/ack scheme becomes a first-class graded completion path, a house-specific
/// fingerprint becomes part of `probe`/`autoDetect`, a new DSL block kind renders through
/// the ordinary pipeline, a template gains a formatter, and a vendor drawer method fills
/// [DrawerKickMethod.vendor] — all per driver, all keyed by namespaced ids
/// (`"acme.x-idle"`), and everything a registration claims is attributed to it BY ID in
/// the job result and in `pdctl verify`. That attribution is the only reason extending the
/// honesty-critical part of this SDK is allowed at all: a custom method's claims are
/// auditable exactly like a built-in's.
///
/// ## Why these take native function pointers rather than Dart closures
///
/// Every callback in this group is invoked on a CORE thread and has to answer *there and
/// then*: a fence must produce bytes before the payload is flushed, a matcher must
/// classify the printer's reply before the next chunk arrives, a formatter must return
/// text while the document is being laid out. `dart:ffi` cannot do that, for exactly the
/// reasons spelled out in [CustomTransport]'s documentation — `isolateLocal` may only be
/// invoked on its own isolate's mutator thread, `listener` supports `void` returns only,
/// and `isolateGroupBound` runs with no isolate entered. A matcher that returned whatever
/// the trampoline left in the return register would be a completion claim made by
/// accident, and this SDK exists to not do that.
///
/// So the callbacks are native, exactly as [CustomTransport]'s connect and write are, and
/// each type below has a `fromLibrary` factory for the usual case: the vendor code sits in
/// a `.so`, a `.dylib` or a framework beside the driver.
///
/// The Swift, Kotlin and .NET wrappers *can* serve these callbacks in their own language
/// and do; this is the one place where the Dart surface is shaped differently from the
/// other three, and it is shaped by the runtime rather than by preference.
library;

import 'dart:ffi';

import 'bindings.dart';
import 'enums.dart';

/// A custom completion mechanism (docs/api.md §16) — the marquee registration point.
///
/// Bind a printer to it by attaching with the profile id `"vendoridle:<id>"`, e.g.
/// `"vendoridle:acme.x-idle"`, which resolves to a generic ESC/POS profile whose completion
/// is this method. The engine sends [fenceBytes]`(token)` behind the payload and routes the
/// printer's response stream through [matcher]; a `Matched(token)` confirms the job exactly
/// like `GS ( H`, with the same per-job token map and the same resolvable verification
/// identifier.
final class CustomCompletionMethod {
  CustomCompletionMethod({
    required this.id,
    required this.fenceBytes,
    required this.matcher,
    this.grade = ConfidenceGrade.aJobLevelConfirmation,
    this.authority = CompletionAuthority.physicalPrinter,
    this.methodName,
    Pointer<Void>? ctx,
  }) : ctx = ctx ?? nullptr;

  /// The same, resolving the two symbols out of an already-open library.
  ///
  /// Throws [ArgumentError] when a symbol is missing, rather than registering a method
  /// that would fail later on a worker thread, where there is no Dart stack to explain it.
  factory CustomCompletionMethod.fromLibrary(
    DynamicLibrary library, {
    required String id,
    required String fenceBytes,
    required String matcher,
    ConfidenceGrade grade = ConfidenceGrade.aJobLevelConfirmation,
    CompletionAuthority authority = CompletionAuthority.physicalPrinter,
    String? methodName,
    Pointer<Void>? ctx,
  }) =>
      CustomCompletionMethod(
        id: id,
        fenceBytes: _resolve<PdFenceBytesNative>(library, fenceBytes),
        matcher: _resolve<PdCompletionMatcherNative>(library, matcher),
        grade: grade,
        authority: authority,
        methodName: methodName,
        ctx: ctx ?? nullptr,
      );

  /// Namespaced id, e.g. `acme.x-idle`.
  final String id;

  /// Produces the fence bytes for a job's four-character token. A fence longer than the
  /// buffer must report its length rather than truncate: the core then fails that job
  /// Unknown instead of sending half a fence.
  final Pointer<NativeFunction<PdFenceBytesNative>> fenceBytes;

  /// Classifies the bytes accumulated since the last verdict.
  final Pointer<NativeFunction<PdCompletionMatcherNative>> matcher;

  /// What a confirmed completion on this method claims.
  final ConfidenceGrade grade;

  /// Who makes that claim.
  final CompletionAuthority authority;

  /// Shown in the result and in `pdctl verify`; null uses [id].
  final String? methodName;

  /// Handed back to both callbacks untouched.
  final Pointer<Void> ctx;
}

/// An extra fingerprinting step for `probe` and `autoDetect` (docs/api.md §16).
///
/// [requestBytes] MUST be non-printing — no `0x20`–`0x7E` run, no line feed. A printing
/// step is refused at registration, because auto-detection must never cost a venue a roll
/// of paper.
final class CustomProbeStep {
  CustomProbeStep({
    required this.id,
    required this.requestBytes,
    required this.classify,
    Pointer<Void>? ctx,
  }) : ctx = ctx ?? nullptr;

  /// The same, resolving `classify` out of an already-open library.
  factory CustomProbeStep.fromLibrary(
    DynamicLibrary library, {
    required String id,
    required List<int> requestBytes,
    required String classify,
    Pointer<Void>? ctx,
  }) =>
      CustomProbeStep(
        id: id,
        requestBytes: requestBytes,
        classify: _resolve<PdProbeClassifyNative>(library, classify),
        ctx: ctx ?? nullptr,
      );

  final String id;

  /// The non-printing request.
  final List<int> requestBytes;

  /// Classifies the response into a `pd_probe_finding`.
  final Pointer<NativeFunction<PdProbeClassifyNative>> classify;

  final Pointer<Void> ctx;
}

/// Renders a new DSL block kind (docs/api.md §16). A handler registered for a kind always
/// owns it: unknown kinds otherwise degrade, and this intercepts first.
///
/// The core stores this and the receipt-DSL render path calls it — but that path has no
/// entry point in `pd.h` yet, so a registration made from Dart is not reached by
/// [Printer.print] today. See docs/api.md §17.1.
final class CustomBlockHandler {
  CustomBlockHandler({
    required this.kind,
    required this.handler,
    Pointer<Void>? ctx,
  }) : ctx = ctx ?? nullptr;

  /// The same, resolving `handler` out of an already-open library.
  factory CustomBlockHandler.fromLibrary(
    DynamicLibrary library, {
    required String kind,
    required String handler,
    Pointer<Void>? ctx,
  }) =>
      CustomBlockHandler(
        kind: kind,
        handler: _resolve<PdBlockHandlerNative>(library, handler),
        ctx: ctx ?? nullptr,
      );

  /// The block object key that selects this handler.
  final String kind;

  /// Writes ESC/POS ops, or declares a degradation — reported exactly like a built-in
  /// block's, never dropped in silence.
  final Pointer<NativeFunction<PdBlockHandlerNative>> handler;

  final Pointer<Void> ctx;
}

/// Backs `{{ v | name:args }}` in the template layer, checked before the built-in table
/// (docs/api.md §16).
///
/// Same reachability caveat as [CustomBlockHandler]: the template layer is not in `pd.h`
/// yet (docs/api.md §17.1).
final class CustomFormatter {
  CustomFormatter({
    required this.name,
    required this.formatter,
    Pointer<Void>? ctx,
  }) : ctx = ctx ?? nullptr;

  /// The same, resolving `formatter` out of an already-open library.
  factory CustomFormatter.fromLibrary(
    DynamicLibrary library, {
    required String name,
    required String formatter,
    Pointer<Void>? ctx,
  }) =>
      CustomFormatter(
        name: name,
        formatter: _resolve<PdFormatterNative>(library, formatter),
        ctx: ctx ?? nullptr,
      );

  /// The formatter name used in templates.
  final String name;

  /// Formats (value, args, locale), or declines and falls through to the built-ins.
  final Pointer<NativeFunction<PdFormatterNative>> formatter;

  final Pointer<Void> ctx;
}

/// A vendor drawer-kick method, filling [DrawerKickMethod.vendor] for a profile
/// (docs/api.md §16, docs/cash-drawer.md).
///
/// [statusRequest] and [statusParse] go together: supplying neither means the method has
/// no readable switch, so a kick reports [DrawerState.kickSentUnverified] rather than a
/// verified open. That is the honest answer, not a weak success.
final class CustomDrawerKick {
  CustomDrawerKick({
    required this.id,
    required this.kickBytes,
    Pointer<NativeFunction<PdDrawerStatusRequestNative>>? statusRequest,
    Pointer<NativeFunction<PdDrawerStatusParseNative>>? statusParse,
    Pointer<Void>? ctx,
  })  : statusRequest = statusRequest ?? nullptr,
        statusParse = statusParse ?? nullptr,
        ctx = ctx ?? nullptr;

  /// The same, resolving the symbols out of an already-open library.
  factory CustomDrawerKick.fromLibrary(
    DynamicLibrary library, {
    required String id,
    required String kickBytes,
    String? statusRequest,
    String? statusParse,
    Pointer<Void>? ctx,
  }) =>
      CustomDrawerKick(
        id: id,
        kickBytes: _resolve<PdDrawerKickBytesNative>(library, kickBytes),
        statusRequest: statusRequest == null
            ? nullptr
            : _resolve<PdDrawerStatusRequestNative>(library, statusRequest),
        statusParse: statusParse == null
            ? nullptr
            : _resolve<PdDrawerStatusParseNative>(library, statusParse),
        ctx: ctx ?? nullptr,
      );

  final String id;

  /// The pulse bytes for (channel, pulse duration).
  final Pointer<NativeFunction<PdDrawerKickBytesNative>> kickBytes;

  /// The bytes that ask for the switch state, or `nullptr`.
  final Pointer<NativeFunction<PdDrawerStatusRequestNative>> statusRequest;

  /// Parses a reply to a pin level (`-1` unknown, `0` low, `1` high), or `nullptr`.
  final Pointer<NativeFunction<PdDrawerStatusParseNative>> statusParse;

  final Pointer<Void> ctx;
}

Pointer<NativeFunction<T>> _resolve<T extends Function>(
    DynamicLibrary library, String symbol) {
  if (!library.providesSymbol(symbol)) {
    throw ArgumentError.value(symbol, 'symbol', 'is not exported by this library');
  }
  return library.lookup<NativeFunction<T>>(symbol);
}

/// A custom matcher's verdict on the printer→host bytes it was handed — `pd_match_kind`.
///
/// Produced by native code rather than by Dart (see the library comment); mirrored here so
/// a diagnostic can name a verdict, and so [PrinterDriver.abiName] can spell it the way
/// the core does.
enum CompletionMatchKind {
  /// A fence answer carrying a token; confirms the job exactly like `GS ( H`.
  matched(0),

  /// Not this mechanism's bytes; the core drops its matcher buffer and reads on.
  notMine(1),

  /// An answer may be forming but is incomplete; the core keeps buffering.
  needMore(2),

  /// A `pd_match_kind` this build does not know.
  unrecognized(-1);

  const CompletionMatchKind(this.nativeValue);

  /// The `pd_match_kind` value.
  final int nativeValue;

  /// The member with this raw value, or [unrecognized].
  static CompletionMatchKind fromNative(int raw) => values.firstWhere(
        (value) => value.nativeValue == raw,
        orElse: () => CompletionMatchKind.unrecognized,
      );
}
