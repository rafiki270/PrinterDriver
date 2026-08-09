/**
 * Custom method registration (docs/api.md §16): five runtime extension points that let an
 * integrator extend the SDK without forking it, all implemented in JavaScript.
 *
 * All are per-driver, all are process-local (never persisted into a shared journal beyond
 * their ids), all are keyed by NAMESPACED string ids ("acme.x-idle"), and everything a
 * registration claims — a completion grade, an authority, a formatter name — is attributed
 * to it by id in the result and in `pdctl verify`. A custom method's claims are auditable
 * exactly like a built-in's.
 *
 * THREADING
 *   Every callback below is invoked from a CORE thread and hopped onto the JS thread by
 *   the native module, which blocks that core thread until the callback answers. So a
 *   callback may be `async` and may touch React state, but it must ANSWER: a callback that
 *   never settles is treated, after the native deadline, as the registration's documented
 *   failure — a `notMine` match, a declined formatter, a degraded block — never as a
 *   success, and never as a hung printer.
 */

import type { ConfidenceGrade, CompletionAuthority, MatchKind } from './enums.ts';

/**
 * A vendor idle/ack scheme as a first-class graded completion path, with no core release.
 * The engine sends `fenceBytes(token)` behind the payload and routes the printer's
 * response stream through `match`; a `matched` verdict confirms the job EXACTLY like a
 * GS ( H echo — same per-job token map, same journalled verification identifier, same
 * `jobByToken` and `pdctl verify` resolution.
 *
 * Select it at attach time with a profile id of the form `vendoridle:<id>`.
 */
export interface CompletionMethodRegistration {
  /** Namespaced, e.g. "acme.x-idle". */
  readonly id: string;
  /** The fence to send behind the payload for this job's four-character token. */
  fenceBytes(jobToken: string): ArrayBuffer | Promise<ArrayBuffer>;
  /**
   * Classify the printer -> host bytes accumulated since the last `matched`/`notMine`.
   * `needMore` keeps buffering; `matched` must carry the four-character token.
   */
  match(data: ArrayBuffer): CompletionMatch | Promise<CompletionMatch>;
  /** What a confirmed completion on this method claims. Claimed, and attributed to `id`. */
  readonly grade: ConfidenceGrade;
  readonly authority: CompletionAuthority;
  /** Shown in the result and in `pdctl verify`. Defaults to `id`. */
  readonly methodName?: string;
}

export type CompletionMatch =
  | { readonly kind: Extract<MatchKind, 'matched'>; readonly token: string }
  | { readonly kind: Exclude<MatchKind, 'matched'> };

/**
 * Extends probe/autoDetect fingerprinting. `requestBytes` MUST be non-printing — no byte
 * in 0x20..0x7E, no line feed — and a printing step is refused at registration, because
 * auto-detection must never cost a venue a roll of paper.
 */
export interface ProbeStepRegistration {
  readonly id: string;
  readonly requestBytes: ArrayBuffer;
  classify(response: ArrayBuffer): ProbeFinding | Promise<ProbeFinding>;
}

export interface ProbeFinding {
  /** True when the device replied to this step at all. */
  readonly answered: boolean;
  /** Short classification, surfaced in the findings summary. Up to 63 characters. */
  readonly label: string;
}

/**
 * A new DSL block kind, rendered through the ordinary pipeline. A handler registered for a
 * kind always owns it. Return the raw ESC/POS ops, or decline with a one-line degradation
 * reason — which is reported exactly the way a built-in block's degradation is.
 */
export interface BlockHandlerRegistration {
  /** The block object key that selects this handler. */
  readonly kind: string;
  render(
    blockJson: string,
    profileJson: string
  ): BlockRenderResult | Promise<BlockRenderResult>;
}

export type BlockRenderResult =
  | { readonly ok: true; readonly bytes: ArrayBuffer }
  | { readonly ok: false; readonly detail: string };

/**
 * Backs `{{ v | name:args }}` in the template layer, checked BEFORE the built-in
 * formatter table. Return null to decline and fall through to the built-ins.
 */
export interface FormatterRegistration {
  readonly name: string;
  format(value: string, args: string, locale: string): string | null | Promise<string | null>;
}

/**
 * Fills `DrawerKickMethod.vendor` for a profile. `statusRequest` and `statusParse` are
 * optional and go together: both absent means the vendor method has no readable switch,
 * so a kick reports `kickSentUnverified` rather than a verified open.
 */
export interface DrawerKickRegistration {
  readonly id: string;
  kickBytes(channel: number, pulseMs: number): ArrayBuffer | Promise<ArrayBuffer>;
  statusRequest?(): ArrayBuffer | Promise<ArrayBuffer>;
  /** Parse a status reply to a pin level. */
  statusParse?(response: ArrayBuffer): PinLevel | Promise<PinLevel>;
}

export type PinLevel = 'unknown' | 'low' | 'high';

export function pinLevelToNative(level: PinLevel): number {
  if (level === 'high') return 1;
  if (level === 'low') return 0;
  return -1;
}

/**
 * The printing-byte lint pd.h enforces at registration, applied here too so an app gets a
 * readable JavaScript error at the call site instead of a `false` from the ABI.
 */
export function assertNonPrinting(bytes: ArrayBuffer, what: string): void {
  const view = new Uint8Array(bytes);
  for (let index = 0; index < view.length; index += 1) {
    const byte = view[index] as number;
    if ((byte >= 0x20 && byte <= 0x7e) || byte === 0x0a) {
      throw new Error(
        `${what}: byte ${index} is 0x${byte.toString(16).padStart(2, '0')}, which is ` +
          'printable. A probe step must never cost a venue a roll of paper, so pd.h ' +
          'refuses printing request bytes at registration.'
      );
    }
  }
}
