/**
 * The tri-state contract: three outcomes, no success boolean, and an exhaustive switch
 * that the compiler enforces.
 *
 * docs/api.md §1.4 — "collapsing `unknown` into either bucket is exactly the bug that
 * produces duplicate kitchen tickets".
 */

import { readFileSync, readdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { ConfidenceGrade, ConfidenceLevel, FailureReason, JobOutcome } from '../src/enums.ts';
import { unmarshalJobResult } from '../src/marshal.ts';
import type { NativeJobResult } from '../src/marshal.ts';
import type { JobResult } from '../src/types.ts';

const rawDone: NativeJobResult = {
  outcome: JobOutcome.done,
  confidence: ConfidenceLevel.cutFaultFree,
  reason: FailureReason.none,
  grade: ConfidenceGrade.aJobLevelConfirmation,
  authority: 0,
  method: 'GS(H) fn48',
};

const rawFailed: NativeJobResult = {
  ...rawDone,
  outcome: JobOutcome.failed,
  confidence: ConfidenceLevel.printerHealthy,
  reason: FailureReason.preflightPaperOut,
  grade: ConfidenceGrade.eTransportOnly,
  method: 'none',
};

const rawUnknown: NativeJobResult = {
  ...rawDone,
  outcome: JobOutcome.unknown,
  confidence: ConfidenceLevel.transportAccepted,
  reason: FailureReason.timeoutAwaitingCompletion,
  grade: ConfidenceGrade.eTransportOnly,
  method: 'none',
};

/**
 * The shape every consuming app is expected to write. The `never` binding in the default
 * branch is what makes forgetting `'unknown'` a COMPILE error rather than a runtime
 * surprise — deleting a case here stops `npm run typecheck`, which is the point of the
 * discriminated union.
 */
function describe(result: JobResult): string {
  switch (result.outcome) {
    case 'done':
      return `done/${result.confidence}/${result.grade}`;
    case 'failed':
      return `failed/${result.reason}`;
    case 'unknown':
      return `unknown/${result.reason}/${result.confidence}`;
    default: {
      const exhaustive: never = result;
      return String(exhaustive);
    }
  }
}

test('a done result carries its evidence and no reason field', () => {
  const result = unmarshalJobResult(rawDone);
  assert.equal(result.outcome, 'done');
  assert.equal(describe(result), 'done/cutFaultFree/aJobLevelConfirmation');
  assert.equal(result.method, 'GS(H) fn48');
  assert.ok(!('reason' in result), 'done must not carry a failure reason');
});

test('a failed result carries the reason', () => {
  const result = unmarshalJobResult(rawFailed);
  assert.equal(result.outcome, 'failed');
  assert.equal(describe(result), 'failed/preflightPaperOut');
});

test('unknown is its own outcome and keeps the confidence reached before it stopped', () => {
  const result = unmarshalJobResult(rawUnknown);
  assert.equal(result.outcome, 'unknown');
  assert.equal(describe(result), 'unknown/timeoutAwaitingCompletion/transportAccepted');
  // The specific bug this SDK exists to prevent: a completion-wait timeout is NOT a
  // failure. Bytes were sent; the receipt may well be printing.
  assert.notEqual(result.outcome, 'failed');
});

test('every outcome value in the ABI is reachable through the union', () => {
  const outcomes = Object.keys(JobOutcome).map((key) =>
    unmarshalJobResult({ ...rawDone, outcome: JobOutcome[key as keyof typeof JobOutcome] })
      .outcome
  );
  assert.deepEqual(outcomes.sort(), ['done', 'failed', 'unknown']);
});

test('no source file offers a success boolean', () => {
  // A grep-level guard, because the whole design collapses the moment someone adds a
  // convenience `isSuccess` on JobResult. Comments are stripped first so this file's own
  // prose about the rule cannot satisfy the rule.
  const here = dirname(fileURLToPath(import.meta.url));
  const sourceDir = resolve(here, '../src');
  const offenders: string[] = [];
  const walk = (directory: string): void => {
    for (const entry of readdirSync(directory, { withFileTypes: true })) {
      const path = join(directory, entry.name);
      if (entry.isDirectory()) {
        walk(path);
        continue;
      }
      if (!entry.name.endsWith('.ts')) continue;
      const code = readFileSync(path, 'utf8')
        .replace(/\/\*[\s\S]*?\*\//g, ' ')
        .replace(/\/\/[^\n]*/g, ' ');
      if (/\b(isSuccess|succeeded|wasSuccessful)\b/.test(code)) offenders.push(path);
      if (/\bsuccess\s*[:?]/.test(code)) offenders.push(path);
    }
  };
  walk(sourceDir);
  assert.deepEqual(offenders, [], 'the tri-state result must never gain a success boolean');
});
