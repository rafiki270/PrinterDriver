/**
 * One event stream type, used for job events, device events, and the discovery /
 * auto-detection feeds.
 *
 * docs/platforms.md maps "job events" to "event emitter keyed by job id, or an async
 * iterator". This is both, over one buffer, because an app should not have to choose:
 *
 *   printer.events.on(event => …)          // callback style
 *   for await (const event of job.events)  // async iterator style
 *
 * Events that arrive before anyone subscribes are buffered, so
 * `for await (const e of job.events)` started a tick after `print()` does not miss the
 * `queued` transition. The buffer is bounded and drops the OLDEST event when it
 * overflows, which is the right end to drop: the terminal event is the one that matters
 * and it is always the newest. `done` closes every open iterator.
 */

export interface Subscription {
  remove(): void;
}

export class EventStream<T> implements AsyncIterable<T> {
  private readonly listeners = new Set<(value: T) => void>();
  private readonly buffer: T[] = [];
  private readonly waiters: ((result: IteratorResult<T>) => void)[] = [];
  private readonly limit: number;
  private closed = false;
  private dropped = 0;

  constructor(bufferLimit = 256) {
    this.limit = bufferLimit;
  }

  /** How many events were dropped because nothing consumed the buffer in time. */
  get droppedCount(): number {
    return this.dropped;
  }

  get isClosed(): boolean {
    return this.closed;
  }

  emit(value: T): void {
    if (this.closed) return;
    const waiter = this.waiters.shift();
    if (waiter !== undefined) {
      waiter({ value, done: false });
    } else {
      this.buffer.push(value);
      while (this.buffer.length > this.limit) {
        this.buffer.shift();
        this.dropped += 1;
      }
    }
    for (const listener of [...this.listeners]) listener(value);
  }

  /** Callback style. The returned subscription removes just this listener. */
  on(listener: (value: T) => void): Subscription {
    this.listeners.add(listener);
    return {
      remove: () => {
        this.listeners.delete(listener);
      },
    };
  }

  /** Resolves with the next event, or null once the stream is closed and drained. */
  next(): Promise<T | null> {
    return this.iterator()
      .next()
      .then((result) => (result.done === true ? null : result.value));
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    for (const waiter of this.waiters.splice(0)) {
      waiter({ value: undefined, done: true });
    }
    this.listeners.clear();
  }

  private iterator(): AsyncIterator<T> {
    return {
      next: (): Promise<IteratorResult<T>> => {
        const buffered = this.buffer.shift();
        if (buffered !== undefined) return Promise.resolve({ value: buffered, done: false });
        if (this.closed) return Promise.resolve({ value: undefined, done: true });
        return new Promise<IteratorResult<T>>((resolve) => {
          this.waiters.push(resolve);
        });
      },
      return: (): Promise<IteratorResult<T>> =>
        Promise.resolve({ value: undefined, done: true }),
    };
  }

  [Symbol.asyncIterator](): AsyncIterator<T> {
    return this.iterator();
  }
}
