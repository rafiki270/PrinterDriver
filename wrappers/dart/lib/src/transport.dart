/// A printer reached over a link the application owns, rather than one the core dials
/// itself (docs/compatibility-brief.md §25).
library;

import 'dart:ffi';

import 'allocation.dart';
import 'bindings.dart';

/// The three operations the core needs in order to drive a printer over a link it knows
/// nothing about: Bluetooth Classic SPP, BLE, MFi, a vendor SDK channel, a USB bulk
/// pipe.
///
/// The split is the point (docs/compatibility-brief.md §25): **the platform owns the
/// socket, the core owns the protocol.** Bluetooth cannot live in a portable C++17 core
/// — on Apple it is CoreBluetooth or ExternalAccessory, on Android a `BluetoothSocket`
/// over RFCOMM, on Linux a BlueZ `AF_BLUETOOTH` socket, each with its own permissions
/// model, pairing UI and threading. Everything that makes this SDK worth using — the
/// ordered fence, the `GS ( H` correlation token, preflight, the journal, the confidence
/// grading — stays on the core's side of this boundary and behaves identically over
/// Bluetooth, TCP or a test double. An application supplying a transport cannot weaken a
/// completion guarantee by accident, because it never makes one.
///
/// ## Why connect and write are function pointers rather than Dart closures
///
/// pd.h invokes all three on the core's worker thread for this printer, one at a time,
/// and two of them must answer *there and then*: `connect` returns whether the link
/// opened, `write` returns how many bytes actually went out. `dart:ffi` offers three
/// ways to put a Dart function behind a C function pointer, and none of them can do
/// that. Each was tried against this ABI, not reasoned about:
///
///   * `NativeCallable.isolateLocal` may only be invoked on the mutator thread of the
///     isolate that created it. Every call the core makes comes from its own worker, and
///     the VM stops the process on the first one: *"Cannot invoke native callback
///     outside an isolate."*
///   * `NativeCallable.listener` is the one kind a foreign thread may invoke, and it
///     supports `Void` returns only — the native caller does not wait for it. Behind
///     `write` it would leave the return register holding whatever the trampoline
///     happened to leave there, and the core would read that as the number of bytes
///     transferred. A garbage positive answers "all of them"; the whole point of this
///     SDK is that "did the bytes go out" is never answered by a guess.
///   * `NativeCallable.isolateGroupBound` is invocable from any thread and does return
///     values, but the SDK marks it experimental, and it runs the callback with no
///     isolate entered: touching a single top-level `int` segfaults the process. A
///     `write` handler cannot avoid touching Dart state — reading the bytes is the job.
///
/// So the vtable holds native pointers, which is also what a real transport already has:
/// the JNI entry points of an Android `BluetoothSocket`, the C shim over a CoreBluetooth
/// delegate, a BlueZ file descriptor. Dart's half of the transport is the other
/// direction — [Printer.feedBytes] and [Printer.reportLinkDropped] — which are ordinary
/// outbound calls and carry no thread restriction beyond the one below.
///
/// ## Thread contract
///
/// pd.h forbids calling `pd_transport_feed_bytes` from inside connect/write/close, since
/// those run on the thread that would have to service the delivery. Through this API
/// that violation is unreachable rather than merely discouraged: no Dart code runs
/// inside connect or write at all, and the one Dart callback available —
/// [CustomTransport.withDartClose] — is a `NativeCallable.listener`, which by
/// construction runs on the isolate's event loop after `close` has already returned.
///
/// The registration outlives individual connections: the core reconnects after a link
/// drop by calling `connect` again, and the application keeps feeding the same
/// [Printer].
final class CustomTransport {
  /// A transport whose three operations are all native code.
  ///
  /// [ctx] is passed back to each of them untouched and must stay alive until the driver
  /// is disposed, as must the code the pointers name; the core copies the vtable but not
  /// what it points at. [close] may be `nullptr`, and the core then never closes the
  /// link itself.
  ///
  /// [description] is what the printer id and the diagnostics derive from, e.g.
  /// `bt-spp:00:11:22:33:44:55`. Null or empty becomes `custom`, which is unambiguous
  /// for one printer and not for two.
  CustomTransport({
    required this.connect,
    required this.write,
    Pointer<NativeFunction<PdTransportCloseNative>>? close,
    Pointer<Void>? ctx,
    this.description,
  })  : close = close ?? nullptr,
        ctx = ctx ?? nullptr,
        _closeCallback = null;

  CustomTransport._({
    required this.connect,
    required this.write,
    required this.close,
    required this.ctx,
    required this.description,
    required NativeCallable<PdTransportCloseNative>? closeCallback,
  }) : _closeCallback = closeCallback;

  /// The same, resolving the three symbols out of an already-open library — the shape an
  /// application's own plugin has, where the Bluetooth code sits in a `.so` or a
  /// framework beside the driver.
  ///
  /// Throws [ArgumentError] when a symbol is missing, rather than registering a
  /// half-built vtable that would fail later on the worker thread, where there is no
  /// Dart stack to explain it.
  factory CustomTransport.fromLibrary(
    DynamicLibrary library, {
    required String connect,
    required String write,
    String? close,
    Pointer<Void>? ctx,
    String? description,
  }) {
    Pointer<NativeFunction<T>> resolve<T extends Function>(String symbol) {
      if (!library.providesSymbol(symbol)) {
        throw ArgumentError.value(
          symbol,
          'symbol',
          'is not exported by this library',
        );
      }
      return library.lookup<NativeFunction<T>>(symbol);
    }

    return CustomTransport(
      connect: resolve<PdTransportConnectNative>(connect),
      write: resolve<PdTransportWriteNative>(write),
      close: close == null ? null : resolve<PdTransportCloseNative>(close),
      ctx: ctx,
      description: description,
    );
  }

  /// Native connect and write, with the close handled in Dart.
  ///
  /// Close is the only one of the three a Dart function may serve, and it is safe for a
  /// specific reason rather than by luck: it returns nothing, so pd.h's "called once per
  /// successful connect, and again is harmless" is satisfied whenever it runs. That
  /// makes `NativeCallable.listener` — the only callback kind a foreign thread may
  /// invoke — exactly right, and [onClose] therefore runs on the isolate's event loop
  /// some time after the core's worker has moved on.
  ///
  /// Use it to release what the platform holds: an `ExternalAccessory` session, a
  /// CoreBluetooth peripheral, an Android socket. Do not use it for anything the *core*
  /// then waits on, because it has already stopped waiting. In particular the link may
  /// still be open for a moment after the core believes it closed it; on the Epson
  /// portables, whose single connection slot is exactly what a close frees, prefer a
  /// native close.
  ///
  /// In practice the core closes a still-connected link exactly once, on
  /// `PrinterDriver.dispose`, so [onClose] usually runs one turn of the event loop
  /// after that call returns.
  factory CustomTransport.withDartClose({
    required Pointer<NativeFunction<PdTransportConnectNative>> connect,
    required Pointer<NativeFunction<PdTransportWriteNative>> write,
    required void Function() onClose,
    Pointer<Void>? ctx,
    String? description,
  }) {
    final callback = NativeCallable<PdTransportCloseNative>.listener(
      (Pointer<Void> ctx) => onClose(),
    );
    // The driver, not this callback, decides when the isolate may exit: a close hook
    // outstanding at the end of a program is the normal state of an idle printer.
    callback.keepIsolateAlive = false;
    return CustomTransport._(
      connect: connect,
      write: write,
      close: callback.nativeFunction,
      ctx: ctx ?? nullptr,
      description: description,
      closeCallback: callback,
    );
  }

  /// Opens the link. Non-zero for success, 0 for failure.
  final Pointer<NativeFunction<PdTransportConnectNative>> connect;

  /// Hands bytes to the link, answering how many actually went out, or a negative value
  /// for a hard failure.
  ///
  /// Report a short write honestly: zero bytes out is a known failure and one byte out
  /// is Unknown (docs/api.md §4), and that difference is what decides whether an
  /// operator should reprint.
  final Pointer<NativeFunction<PdTransportWriteNative>> write;

  /// Closes the link, or `nullptr` when the application closes it on its own terms.
  final Pointer<NativeFunction<PdTransportCloseNative>> close;

  /// Handed back to each operation untouched.
  final Pointer<Void> ctx;

  /// What the printer id derives from when none is supplied.
  final String? description;

  final NativeCallable<PdTransportCloseNative>? _closeCallback;

  /// Writes this into a zeroed `pd_transport_vtable`.
  void fillNative(Arena arena, Pointer<PdTransportVtable> out) {
    out.ref
      ..connect = connect
      ..write = write
      ..close = close
      ..description = arena.string(description);
  }

  /// Releases the Dart close hook, if there is one. The driver calls this from
  /// `dispose()`, once the core can no longer invoke it.
  void release() => _closeCallback?.close();
}
