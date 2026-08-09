/// The struct layouts, pinned against what a C compiler produces for pd.h.
///
/// `dart:ffi` lays a [Struct] out with the same natural-alignment rules a C compiler
/// uses, which means the bindings are correct exactly as long as the field order
/// matches pd.h. These sizes come from `sizeof` on this platform's compiler; a field
/// inserted in the wrong place changes one of them.
library;

import 'dart:ffi';
import 'dart:typed_data';

import 'package:printerdriver/printerdriver.dart';
import 'package:printerdriver/src/bindings.dart';
import 'package:test/test.dart';

void main() {
  group('ABI layout', () {
    test('every mirrored struct is the size pd.h compiles to', () {
      expect(sizeOf<PdJobEvent>(), 24);
      // outcome, confidence, reason, grade, authority (5 x int32), then the method
      // pointer on its own 8-byte boundary.
      expect(sizeOf<PdJobResult>(), 32);
      expect(sizeOf<PdDeviceStatus>(), 36);
      expect(sizeOf<PdRasterRgba8>(), 32);
      expect(sizeOf<PdOp>(), 24);
      expect(sizeOf<PdDocument>(), 24);
      expect(sizeOf<PdRaw>(), 16);
      expect(sizeOf<PdPayload>(), 40);
      expect(sizeOf<PdConfig>(), 32);
      expect(sizeOf<PdTcpConfig>(), 40);
      expect(sizeOf<PdJobOptions>(), 40);
      expect(sizeOf<PdReprintOptions>(), 48);
      // Three function pointers and the description.
      expect(sizeOf<PdTransportVtable>(), 32);
    });

    test('the payload union is one storage area shared by the three tiers', () {
      // 8 for the tagged kind plus padding, then the largest member, the raster tier.
      expect(sizeOf<PdPayload>(), 8 + sizeOf<PdRasterRgba8>());
      expect(sizeOf<PdPayloadAs>(), sizeOf<PdRasterRgba8>());
    });

    test('grayscale bytes expand to the RGBA8 the raster tier takes', () {
      final payload = Payload.grayscale(
        Uint8List.fromList(<int>[0, 128, 255, 7]),
        width: 2,
        height: 2,
      ) as RasterPayload;

      expect(payload.kind, PayloadKind.rasterRgba8);
      expect(payload.rgba8, hasLength(16));
      // Opaque grey: the core's ITU-R 601 weights sum to exactly 256, so the grey it
      // recovers from (g, g, g, 255) is the byte that went in.
      expect(payload.rgba8.sublist(0, 4), <int>[0, 0, 0, 255]);
      expect(payload.rgba8.sublist(4, 8), <int>[128, 128, 128, 255]);
      expect(payload.rgba8.sublist(8, 12), <int>[255, 255, 255, 255]);
      expect(payload.rgba8.sublist(12, 16), <int>[7, 7, 7, 255]);
    });

    test('a grayscale buffer shorter than the frame is refused, not padded',
        () {
      expect(
        () => Payload.grayscale(Uint8List(3), width: 2, height: 2),
        throwsArgumentError,
      );
    });

    test('a feed op outside 1..255 is refused before it reaches the ABI', () {
      expect(() => DocumentOp.feed(0), throwsRangeError);
      expect(() => DocumentOp.feed(256), throwsRangeError);
      expect(DocumentOp.feed(1), isA<DocumentOp>());
    });
  });
}
