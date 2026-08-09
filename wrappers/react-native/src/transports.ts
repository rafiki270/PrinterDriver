/**
 * Custom transports: THE PLATFORM OWNS THE SOCKET, THE CORE OWNS THE PROTOCOL.
 *
 * Bluetooth cannot live in a portable C++17 core and should not — on Apple it is
 * CoreBluetooth or ExternalAccessory, on Android a BluetoothSocket over RFCOMM, each with
 * its own permissions model, pairing UI and threading. In React Native the natural owner
 * of that link is the JavaScript side, which already has a BLE library. So an app
 * implements three operations here and pushes received bytes in with
 * `printer.feedBytes(...)`, and everything that makes this SDK worth using — the ordered
 * fence, the GS ( H correlation token, preflight, the journal, the confidence grading,
 * the refusal to overclaim — stays on the core's side of the boundary and behaves
 * identically over Bluetooth, TCP or a test double.
 *
 * A wrapper cannot weaken a completion guarantee by accident, because a wrapper never
 * makes one.
 *
 * THREAD CONTRACT (pd.h, "Custom transports")
 *   - `connect`, `write` and `close` are invoked one at a time, never concurrently with
 *     each other. The core calls them on the printer's worker thread; the native module
 *     hops each call onto the JS thread and blocks that worker until you answer, so your
 *     implementation may be async and may touch React state.
 *   - `printer.feedBytes(...)` may be called at ANY time, including while a `write` is in
 *     flight. That is the normal case: a status answer arrives while the next chunk goes
 *     out. Do NOT call it from inside `connect`/`write`/`close`.
 *   - The registration outlives individual connections. After a link drop the core calls
 *     `connect` again; keep feeding the same printer.
 *
 * HONEST SHORT WRITES
 *   `write` returns the number of bytes actually transferred, or a negative number for a
 *   hard failure. Round nothing up. Zero bytes out is a known failure and one byte out is
 *   Unknown (docs/api.md §4), and that difference is what decides whether an operator
 *   should reprint.
 */

export interface CustomTransport {
  /**
   * What the printer id and the diagnostics derive from, e.g.
   * "bt-spp:00:11:22:33:44:55". Omitted becomes "custom", which is fine for one printer
   * and ambiguous for two.
   */
  readonly description?: string;

  /** Open the link. Resolve true for success. */
  connect(): boolean | Promise<boolean>;

  /** Hand the bytes to the link. Resolve the count actually transferred, or a negative. */
  write(data: ArrayBuffer): number | Promise<number>;

  /** Close the link. Called once per successful connect; being called again is harmless. */
  close(): void | Promise<void>;
}

/** @internal The wire answers `respond` expects for each transport question. */
export interface TransportAnswers {
  connect: { ok: boolean };
  write: { written: number };
  close: Record<string, never>;
}
