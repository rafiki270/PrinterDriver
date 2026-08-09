/**
 * Payload encoding and option marshalling — the two places a wrapper can silently change
 * what gets printed.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  Alignment,
  Binarization,
  CodePage,
  CutSetting,
  DrainOrder,
  OpKind,
  PreflightMode,
} from '../src/enums.ts';
import {
  PayloadError,
  marshalDocumentOp,
  marshalDrawerRequest,
  marshalDriverConfig,
  marshalJobOptions,
  marshalPayload,
  marshalQueueOptions,
  marshalQueuePolicy,
  marshalReprintOptions,
  marshalSelfTestOptions,
  marshalTcpConfig,
  unmarshalDeviceStatus,
  unmarshalDrawerReading,
  unmarshalJobEvent,
} from '../src/marshal.ts';
import { Payloads, Receipt, toArrayBuffer } from '../src/payload.ts';

// --- job options ------------------------------------------------------------------------

test('an empty JobOptions marshals to the all-zeroes ABI default', () => {
  assert.deepEqual(marshalJobOptions(undefined), {
    key: '',
    cut: CutSetting.profile,
    openDrawer: 0,
    preflight: PreflightMode.strict,
    timeoutMs: 0,
    topFeedDots: 0,
    bottomFeedDots: 0,
    suppressVerificationId: 0,
  });
});

test('printVerificationId is inverted exactly once on the way out', () => {
  // pd.h inverts the flag so that an all-zeroes pd_job_options still prints the evidence.
  // The inversion lives here and nowhere else.
  assert.equal(marshalJobOptions({}).suppressVerificationId, 0);
  assert.equal(marshalJobOptions({ printVerificationId: true }).suppressVerificationId, 0);
  assert.equal(marshalJobOptions({ printVerificationId: false }).suppressVerificationId, 1);
});

test('the reprint banner is inverted the same way and defaults to printing', () => {
  assert.equal(marshalReprintOptions(undefined).suppressBanner, 0);
  assert.equal(marshalReprintOptions({ banner: true }).suppressBanner, 0);
  assert.equal(marshalReprintOptions({ banner: false }).suppressBanner, 1);
  assert.equal(marshalReprintOptions({ key: 'order-1' }).job.key, 'order-1');
});

test('job options carry margins and the cut setting through unchanged', () => {
  const marshalled = marshalJobOptions({
    key: 'order-7F3A#kitchen-1',
    cut: 'full',
    openDrawer: true,
    preflight: 'skip',
    timeoutMs: 9000,
    topFeedDots: 24,
    bottomFeedDots: 96,
  });
  assert.deepEqual(marshalled, {
    key: 'order-7F3A#kitchen-1',
    cut: CutSetting.full,
    openDrawer: 1,
    preflight: PreflightMode.skip,
    timeoutMs: 9000,
    topFeedDots: 24,
    bottomFeedDots: 96,
    suppressVerificationId: 0,
  });
});

test('driver, tcp, queue, drawer and self-test options all zero-fill', () => {
  assert.deepEqual(marshalDriverConfig(undefined), {
    storageDirectory: '',
    fsyncDisabled: 0,
    hasLog: 0,
  });
  assert.equal(marshalDriverConfig({ onLog: () => {} }).hasLog, 1);
  assert.deepEqual(marshalTcpConfig({ host: '192.168.1.101' }), {
    printerId: '',
    host: '192.168.1.101',
    port: 0,
    widthDots: 0,
    profileId: '',
    connectTimeoutMs: 0,
  });
  assert.deepEqual(marshalQueuePolicy(undefined), {
    holdWhileOffline: 0,
    defaultTtlMs: 0,
    maxDepth: 0,
    drainOrder: DrainOrder.fifo,
  });
  assert.equal(marshalQueuePolicy({ drainOrder: 'priority' }).drainOrder, DrainOrder.priority);
  assert.equal(marshalQueueOptions({ priority: 3 }).priority, 3);
  assert.deepEqual(marshalDrawerRequest(undefined), { channel: 0, pulseMs: 0 });
  assert.equal(marshalSelfTestOptions({ noBarcode: true }).noBarcode, 1);
  assert.equal(marshalSelfTestOptions(undefined).barcodeData, '');
});

// --- payload tiers -----------------------------------------------------------------------

test('tier 2: the text helper produces one line op per string', () => {
  const payload = Payloads.text(['MY RESTAURANT', 'Order 7F3A-92C1']);
  const marshalled = marshalPayload(payload);
  assert.equal(marshalled.kind, 1);
  assert.deepEqual(marshalled, {
    kind: 1,
    codePage: CodePage.pc437,
    ops: [
      { kind: OpKind.line, text: 'MY RESTAURANT', value: 0 },
      { kind: OpKind.line, text: 'Order 7F3A-92C1', value: 0 },
    ],
  });
});

test('tier 2: the Receipt builder encodes style ops with the ESC operands', () => {
  const payload = new Receipt('pc852')
    .align('center')
    .bold()
    .line('MY RESTAURANT')
    .bold(false)
    .align('left')
    .text('no break')
    .line()
    .feed(3)
    .build();
  const marshalled = marshalPayload(payload);
  assert.deepEqual(marshalled, {
    kind: 1,
    codePage: CodePage.pc852,
    ops: [
      { kind: OpKind.align, text: null, value: Alignment.center },
      { kind: OpKind.bold, text: null, value: 1 },
      { kind: OpKind.line, text: 'MY RESTAURANT', value: 0 },
      { kind: OpKind.bold, text: null, value: 0 },
      { kind: OpKind.align, text: null, value: Alignment.left },
      { kind: OpKind.text, text: 'no break', value: 0 },
      { kind: OpKind.line, text: null, value: 0 },
      { kind: OpKind.feed, text: null, value: 3 },
    ],
  });
});

test('tier 2: a bare line() is a null text, which pd.h reads as a bare LF', () => {
  assert.deepEqual(marshalDocumentOp({ kind: 'line' }), {
    kind: OpKind.line,
    text: null,
    value: 0,
  });
  assert.deepEqual(marshalDocumentOp({ kind: 'line', text: '' }), {
    kind: OpKind.line,
    text: '',
    value: 0,
  });
});

test('tier 2: feed is refused outside 1..255 instead of wrapping on the wire', () => {
  assert.throws(() => marshalDocumentOp({ kind: 'feed', lines: 0 }), PayloadError);
  assert.throws(() => marshalDocumentOp({ kind: 'feed', lines: 256 }), PayloadError);
  assert.throws(() => marshalDocumentOp({ kind: 'feed', lines: 1.5 }), PayloadError);
  assert.equal(marshalDocumentOp({ kind: 'feed', lines: 255 }).value, 255);
});

test('tier 1: raster carries the buffer itself, not a copy', () => {
  const pixels = new ArrayBuffer(4 * 4 * 2);
  const payload = Payloads.raster({ pixels, width: 4, height: 2 });
  const marshalled = marshalPayload(payload);
  assert.equal(marshalled.kind, 0);
  if (marshalled.kind !== 0) throw new Error('unreachable');
  assert.equal(marshalled.pixels, pixels, 'the ArrayBuffer must cross by identity');
  assert.equal(marshalled.strideBytes, 0);
  assert.equal(marshalled.binarization, Binarization.fixedThreshold);
  assert.equal(marshalled.threshold, 0);
  assert.equal(marshalled.maxRowsPerBand, 0);
});

test('tier 1: a typed-array view is copied, because a view may window a shared pool', () => {
  const pool = new Uint8Array(64);
  pool.fill(7);
  const view = pool.subarray(16, 48);
  const payload = Payloads.raster({ pixels: view, width: 4, height: 2 });
  assert.notEqual(payload.pixels, pool.buffer);
  assert.equal(payload.pixels.byteLength, 32);
  assert.deepEqual([...new Uint8Array(payload.pixels)], Array<number>(32).fill(7));
});

test('tier 1: a pixel buffer too small for the stated size is refused before submission', () => {
  const short = new ArrayBuffer(4 * 4 * 2 - 1);
  assert.throws(
    () => marshalPayload(Payloads.raster({ pixels: short, width: 4, height: 2 })),
    PayloadError
  );
  // A stride narrower than one RGBA8 row cannot be honest either.
  assert.throws(
    () =>
      marshalPayload(
        Payloads.raster({ pixels: new ArrayBuffer(256), width: 4, height: 2, strideBytes: 8 })
      ),
    PayloadError
  );
  // A wider stride is fine, as long as the buffer holds the rows it implies.
  const strided = marshalPayload(
    Payloads.raster({ pixels: new ArrayBuffer(64), width: 4, height: 2, strideBytes: 32 })
  );
  assert.equal(strided.kind, 0);
});

test('tier 1: floyd-steinberg and a threshold survive marshalling', () => {
  const marshalled = marshalPayload(
    Payloads.raster({
      pixels: new ArrayBuffer(4 * 2 * 4),
      width: 2,
      height: 4,
      binarization: 'floydSteinberg',
      threshold: 200,
      maxRowsPerBand: 512,
    })
  );
  if (marshalled.kind !== 0) throw new Error('unreachable');
  assert.equal(marshalled.binarization, Binarization.floydSteinberg);
  assert.equal(marshalled.threshold, 200);
  assert.equal(marshalled.maxRowsPerBand, 512);
});

test('tier 3: raw bytes cross verbatim and an empty payload is refused', () => {
  const bytes = Uint8Array.from([0x1b, 0x40, 0x41]);
  const payload = Payloads.raw(bytes);
  const marshalled = marshalPayload(payload);
  assert.equal(marshalled.kind, 2);
  if (marshalled.kind !== 2) throw new Error('unreachable');
  assert.deepEqual([...new Uint8Array(marshalled.bytes)], [0x1b, 0x40, 0x41]);
  assert.throws(() => marshalPayload(Payloads.raw(new ArrayBuffer(0))), PayloadError);
});

test('toArrayBuffer passes a whole buffer through untouched', () => {
  const buffer = new ArrayBuffer(8);
  assert.equal(toArrayBuffer(buffer), buffer);
});

// --- inbound structs ----------------------------------------------------------------------

test('a device status that has never heard from the device says so', () => {
  const status = unmarshalDeviceStatus({
    connected: 1,
    observed: 0,
    online: -1,
    coverOpen: -1,
    paperOut: -1,
    paperNearEnd: -1,
    cutterError: -1,
    unrecoverableError: -1,
    recoverableError: -1,
  });
  assert.equal(status.connected, true);
  assert.equal(status.observed, false);
  assert.equal(status.online, 'unknown');
  assert.equal(status.paperOut, 'unknown');
});

test('a job event only carries a reason when the ABI flagged one', () => {
  const plain = unmarshalJobEvent({
    state: 2,
    confidence: 0,
    hasReason: 0,
    reason: 4,
    monotonicMs: 12,
  });
  assert.ok(!('reason' in plain));
  assert.equal(plain.state, 'sendStarted');
  const failed = unmarshalJobEvent({
    state: 8,
    confidence: 1,
    hasReason: 1,
    reason: 3,
    monotonicMs: 99,
  });
  assert.equal(failed.reason, 'preflightPaperOut');
  assert.equal(failed.monotonicMs, 99);
});

test('an uncalibrated drawer reading stays unknown however clear the level is', () => {
  const reading = unmarshalDrawerReading({
    available: 1,
    answered: 1,
    pinHigh: 1,
    needsCalibration: 1,
    state: 7,
  });
  assert.equal(reading.pinHigh, 'yes');
  assert.equal(reading.needsCalibration, true);
  assert.equal(reading.state, 'unknown');
});
