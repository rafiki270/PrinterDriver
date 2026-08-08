import 'dart:convert';
import 'dart:ffi';
import 'dart:typed_data';

/// Native scratch memory for the duration of one ABI call.
///
/// `package:ffi` would supply this, but it would also be the package's only runtime
/// dependency, and everything needed here is `calloc`/`free` from the C library the
/// process is already linked against. Everything handed to the ABI is short-lived by
/// contract: pd.h copies every `const char*` and every buffer before the call returns,
/// so an arena released right after the call is exactly the right lifetime.
final class Arena {
  final List<Pointer<NativeType>> _blocks = <Pointer<NativeType>>[];

  static final DynamicLibrary _process = DynamicLibrary.process();

  static final Pointer<Void> Function(int count, int size) _calloc =
      _process.lookupFunction<Pointer<Void> Function(Size, Size),
          Pointer<Void> Function(int, int)>(
    'calloc',
  );

  static final void Function(Pointer<Void>) _free = _process.lookupFunction<
      Void Function(Pointer<Void>), void Function(Pointer<Void>)>(
    'free',
  );

  /// [byteCount] zeroed bytes. Zeroed rather than raw because pd.h documents
  /// all-zeroes as the valid default for every struct it takes.
  Pointer<T> allocate<T extends NativeType>(int byteCount) {
    if (byteCount <= 0) return nullptr;
    final block = _calloc(byteCount, 1);
    if (block == nullptr) {
      throw OutOfMemoryError();
    }
    _blocks.add(block);
    return block.cast<T>();
  }

  /// A NUL-terminated UTF-8 copy of [value], or `nullptr` for null.
  ///
  /// Null and the empty string are not interchangeable in this ABI — `pd_job_options.key`
  /// treats both as "generate one", but `pd_tcp_config.host` rejects both — so the
  /// distinction is preserved rather than normalised here.
  Pointer<Char> string(String? value) {
    if (value == null) return nullptr;
    final encoded = utf8.encode(value);
    final buffer = allocate<Uint8>(encoded.length + 1);
    buffer.asTypedList(encoded.length + 1)
      ..setRange(0, encoded.length, encoded)
      ..[encoded.length] = 0;
    return buffer.cast<Char>();
  }

  /// A native copy of [bytes].
  Pointer<Uint8> bytes(List<int> bytes) {
    if (bytes.isEmpty) return nullptr;
    final buffer = allocate<Uint8>(bytes.length);
    buffer.asTypedList(bytes.length).setRange(0, bytes.length, bytes);
    return buffer;
  }

  /// Frees everything this arena handed out. Idempotent.
  void releaseAll() {
    for (final block in _blocks) {
      _free(block.cast<Void>());
    }
    _blocks.clear();
  }

  /// Runs [body] with an arena that is released however [body] ends.
  static R using<R>(R Function(Arena arena) body) {
    final arena = Arena();
    try {
      return body(arena);
    } finally {
      arena.releaseAll();
    }
  }
}

/// Reads a NUL-terminated UTF-8 string the ABI owns.
///
/// Never frees anything: pd.h is explicit that nothing it returns is freed by the
/// caller.
String readNativeString(Pointer<Char> pointer) {
  if (pointer == nullptr) return '';
  final bytes = pointer.cast<Uint8>();
  var length = 0;
  while (bytes[length] != 0) {
    length++;
  }
  if (length == 0) return '';
  return utf8.decode(Uint8List.fromList(bytes.asTypedList(length)));
}
