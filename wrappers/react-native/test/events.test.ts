/**
 * The event stream is both an emitter and an async iterator over one buffer, because
 * docs/platforms.md maps job events to "event emitter keyed by job id, OR an async
 * iterator" and an app should not have to choose.
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { EventStream } from '../src/events.ts';

test('events emitted before anyone subscribes are still delivered to an iterator', async () => {
  const stream = new EventStream<number>();
  stream.emit(1);
  stream.emit(2);
  stream.close();
  const seen: number[] = [];
  for await (const value of stream) seen.push(value);
  assert.deepEqual(seen, [1, 2]);
});

test('a listener sees events as they arrive and can remove itself', () => {
  const stream = new EventStream<string>();
  const seen: string[] = [];
  const subscription = stream.on((value) => seen.push(value));
  stream.emit('a');
  subscription.remove();
  stream.emit('b');
  assert.deepEqual(seen, ['a']);
});

test('an iterator waiting on an empty stream resumes when an event arrives', async () => {
  const stream = new EventStream<number>();
  const pending = stream.next();
  stream.emit(42);
  assert.equal(await pending, 42);
});

test('closing resolves a waiting iterator with null rather than hanging', async () => {
  const stream = new EventStream<number>();
  const pending = stream.next();
  stream.close();
  assert.equal(await pending, null);
  assert.equal(stream.isClosed, true);
});

test('emitting after close is a no-op', async () => {
  const stream = new EventStream<number>();
  stream.close();
  stream.emit(1);
  assert.equal(await stream.next(), null);
});

test('the buffer drops the OLDEST event on overflow, never the newest', async () => {
  // The terminal event is the one that matters and it is always the newest, so an
  // unattended stream must not be able to lose it.
  const stream = new EventStream<number>(3);
  for (const value of [1, 2, 3, 4, 5]) stream.emit(value);
  stream.close();
  const seen: number[] = [];
  for await (const value of stream) seen.push(value);
  assert.deepEqual(seen, [3, 4, 5]);
  assert.equal(stream.droppedCount, 2);
});

test('two iterators share the buffer rather than duplicating it', async () => {
  // Deliberate: the stream is a queue, not a broadcast. `on()` is the broadcast form.
  const stream = new EventStream<number>();
  stream.emit(1);
  stream.emit(2);
  const first = await stream.next();
  const second = await stream.next();
  assert.deepEqual([first, second], [1, 2]);
});
