/**
 * The sink router: how everything the native side sends reaches JavaScript, and how the
 * questions it asks get answered.
 *
 * The failure modes tested here are the ones that hang a printer rather than throwing an
 * error, so they are the ones worth pinning down: a handler that is missing, a handler
 * that throws, a handler that rejects, and a message kind this build has never heard of.
 * Every one of them must still produce exactly one `respond`.
 */

import { test, beforeEach } from 'node:test';
import assert from 'node:assert/strict';

import {
  addDeviceListener,
  addJobListener,
  addLogListener,
  dispatchSinkMessage,
  installSinkForTesting,
  openStream,
  resetBridgeForTesting,
  setNativeModule,
  setRequestHandler,
} from '../src/native.ts';
import { createFakeNative } from './support/fakeNative.ts';
import type { FakeNative } from './support/fakeNative.ts';

let fake: FakeNative;

beforeEach(() => {
  resetBridgeForTesting();
  fake = createFakeNative();
  setNativeModule(fake.module);
  installSinkForTesting();
});

test('the sink is installed on the native module exactly once', () => {
  assert.notEqual(fake.sink, null);
});

test('a job message reaches only the listener for that job handle', () => {
  const seen: number[] = [];
  addJobListener(100, (message) => seen.push(message.state));
  addJobListener(101, () => assert.fail('the wrong job listener fired'));
  fake.sink?.('job', {
    job: 100,
    state: 4,
    confidence: 2,
    hasReason: 0,
    reason: 0,
    monotonicMs: 5,
  });
  assert.deepEqual(seen, [4]);
});

test('removing a job listener stops delivery', () => {
  let count = 0;
  const remove = addJobListener(1, () => {
    count += 1;
  });
  dispatchSinkMessage('job', { job: 1, state: 0, confidence: 0, hasReason: 0, reason: 0, monotonicMs: 0 });
  remove();
  dispatchSinkMessage('job', { job: 1, state: 0, confidence: 0, hasReason: 0, reason: 0, monotonicMs: 0 });
  assert.equal(count, 1);
});

test('a device message reaches the listener for that printer handle', () => {
  const seen: number[] = [];
  addDeviceListener(10, (message) => seen.push(message.event));
  dispatchSinkMessage('device', { printer: 10, event: 4 });
  assert.deepEqual(seen, [4]);
});

test('log messages reach every log listener', () => {
  const seen: string[] = [];
  addLogListener((message) => seen.push(message));
  dispatchSinkMessage('log', { message: 'connect failed' });
  assert.deepEqual(seen, ['connect failed']);
});

test('a discovery stream only sees its own requestId', () => {
  const mine: unknown[] = [];
  const stream = openStream((payload) => mine.push(payload.device));
  dispatchSinkMessage('discovered', { requestId: stream.requestId, device: { ip: '10.0.0.1' } });
  dispatchSinkMessage('discovered', { requestId: stream.requestId + 99, device: { ip: '10.0.0.2' } });
  assert.deepEqual(mine, [{ ip: '10.0.0.1' }]);
  stream.close();
  dispatchSinkMessage('discovered', { requestId: stream.requestId, device: { ip: '10.0.0.3' } });
  assert.equal(mine.length, 1);
});

test('a question with a registered handler is answered with what it returned', () => {
  setRequestHandler('transport.write', 7, () => ({ written: 42 }));
  dispatchSinkMessage('transport.write', { requestId: 3, transport: 7, data: new ArrayBuffer(2) });
  assert.deepEqual(fake.responses, [{ requestId: 3, value: { written: 42 } }]);
});

test('a question with NO handler still gets exactly one answer: the documented failure', () => {
  dispatchSinkMessage('transport.connect', { requestId: 4, transport: 99 });
  assert.deepEqual(fake.responses, [{ requestId: 4, value: { ok: false } }]);
});

test('a handler that throws falls back to the documented failure, not to silence', () => {
  setRequestHandler('completion.match', 'acme.x-idle', () => {
    throw new Error('boom');
  });
  dispatchSinkMessage('completion.match', {
    requestId: 5,
    id: 'acme.x-idle',
    data: new ArrayBuffer(0),
  });
  // notMine (1) with no token: the core drops its matcher buffer and the job is not
  // confirmed. A thrown handler must never look like a match.
  assert.deepEqual(fake.responses, [{ requestId: 5, value: { kind: 1, token: '' } }]);
});

test('an async handler answers when its promise settles', async () => {
  setRequestHandler('transport.write', 7, async () => {
    await Promise.resolve();
    return { written: 3 };
  });
  dispatchSinkMessage('transport.write', { requestId: 6, transport: 7, data: new ArrayBuffer(3) });
  assert.deepEqual(fake.responses, []);
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(fake.responses, [{ requestId: 6, value: { written: 3 } }]);
});

test('a rejected promise falls back rather than leaving the core thread parked', async () => {
  setRequestHandler('transport.write', 7, () => Promise.reject(new Error('link gone')));
  dispatchSinkMessage('transport.write', { requestId: 7, transport: 7, data: new ArrayBuffer(1) });
  await new Promise((resolve) => setImmediate(resolve));
  // -1 is pd.h's hard failure for pd_transport_write_fn, not 0: zero bytes out is a known
  // failure and a hard error is a different fact.
  assert.deepEqual(fake.responses, [{ requestId: 7, value: { written: -1 } }]);
});

test('an unrecognised kind carrying a requestId is still answered', () => {
  // The native library being newer than the JS package must not hang a printer.
  dispatchSinkMessage('something.new', { requestId: 8 });
  assert.deepEqual(fake.responses, [{ requestId: 8, value: {} }]);
});

test('an unrecognised kind with no requestId is dropped silently', () => {
  dispatchSinkMessage('something.new', {});
  assert.deepEqual(fake.responses, []);
});

test('handlers are keyed per registration id, so two never collide', () => {
  setRequestHandler('drawer.kickBytes', 'acme.one', () => ({ bytes: new ArrayBuffer(1) }));
  setRequestHandler('drawer.kickBytes', 'acme.two', () => ({ bytes: new ArrayBuffer(2) }));
  dispatchSinkMessage('drawer.kickBytes', { requestId: 9, id: 'acme.two' });
  const answered = fake.responses[0]?.value as { bytes: ArrayBuffer };
  assert.equal(answered.bytes.byteLength, 2);
});
