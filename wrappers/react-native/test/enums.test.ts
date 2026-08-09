/**
 * The enum bridge test every wrapper in this repository carries, in its React Native form.
 *
 * It compares the hand-written mirrors in src/enums.ts against
 * src/generated/abi.generated.ts, which scripts/generate-abi-mirror.mjs extracts from
 * capi/include/printerdriver/pd.h. A value added to the core without updating this
 * package fails here rather than turning into "not implemented on platform X".
 */

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { generatedEnums } from '../src/generated/abi.generated.ts';
import {
  ConfidenceGrade,
  JobOutcome,
  JobState,
  PD_FALSE,
  PD_TRUE,
  PD_UNKNOWN,
  UnknownEnumValueError,
  codePageOrder,
  confidenceGradeLetters,
  enumMirrors,
  jobStateFromNative,
  tristateFromNative,
  tristateToNative,
} from '../src/enums.ts';

test('every pd.h enum has a mirror, and no mirror is invented', () => {
  const generated = new Set(generatedEnums.map((item) => item.name));
  const mirrored = new Set(Object.keys(enumMirrors));
  assert.deepEqual(
    [...generated].filter((name) => !mirrored.has(name)),
    [],
    'pd.h declares an enum this package does not mirror'
  );
  assert.deepEqual(
    [...mirrored].filter((name) => !generated.has(name)),
    [],
    'this package mirrors an enum pd.h does not declare'
  );
  assert.ok(generated.size >= 28, `expected at least 28 enums, saw ${generated.size}`);
});

for (const item of generatedEnums) {
  test(`${item.name}: members and values match pd.h`, () => {
    const mirror = enumMirrors[item.name];
    assert.ok(mirror !== undefined, `${item.name} is not mirrored`);
    const expected = Object.fromEntries(item.members.map((member) => [member.key, member.value]));
    assert.deepEqual(
      mirror,
      expected,
      `${item.name} drifted from pd.h; regenerate with npm run abi:generate and fix src/enums.ts`
    );
  });

  test(`${item.name}: ${item.countName} equals the member count`, () => {
    const mirror = enumMirrors[item.name] as Record<string, number>;
    assert.equal(Object.keys(mirror).length, item.count);
  });
}

test('code page values are the ESC t operand, not indices', () => {
  // pd.h is explicit that pd_code_page is non-contiguous and PD_CODE_PAGE_COUNT is a
  // member count rather than a past-the-end sentinel. A mirror that silently renumbered
  // them to 0..4 would still "match a count" but would select the wrong character set.
  const codePage = enumMirrors.pd_code_page as Record<string, number>;
  assert.deepEqual(Object.values(codePage), [0, 2, 16, 18, 19]);
  assert.equal(codePageOrder.length, Object.keys(codePage).length);
  for (const key of codePageOrder) assert.ok(key in codePage);
});

test('the completion mechanism mirror keeps the M13b members at the end', () => {
  // pd.h: "a new member may only ever go at the end; inserting one beside its conceptual
  // neighbours would renumber every mirror."
  const mechanism = enumMirrors.pd_completion_mechanism as Record<string, number>;
  assert.equal(mechanism.none, 5);
  assert.equal(mechanism.starEtb, 6);
  assert.equal(mechanism.starEscGsEtx, 7);
});

test('grade letters cover every grade exactly once', () => {
  assert.deepEqual(Object.keys(confidenceGradeLetters).sort(), Object.keys(ConfidenceGrade).sort());
  assert.deepEqual(Object.values(confidenceGradeLetters), ['A+', 'A', 'B', 'C', 'D', 'E']);
});

test('a decoder reports an unknown native value rather than defaulting', () => {
  assert.equal(jobStateFromNative(JobState.doneSoftware), 'doneSoftware');
  assert.throws(() => jobStateFromNative(99), UnknownEnumValueError);
  try {
    jobStateFromNative(99);
  } catch (error) {
    assert.ok(error instanceof UnknownEnumValueError);
    assert.equal(error.enumName, 'pd_job_state');
    assert.equal(error.value, 99);
    assert.match(error.message, /newer than the JavaScript package/);
  }
});

test('the tri-state helpers keep unknown distinct from false', () => {
  assert.equal(tristateFromNative(PD_UNKNOWN), 'unknown');
  assert.equal(tristateFromNative(PD_FALSE), 'no');
  assert.equal(tristateFromNative(PD_TRUE), 'yes');
  assert.equal(tristateToNative('unknown'), PD_UNKNOWN);
  assert.equal(tristateToNative('no'), PD_FALSE);
  assert.equal(tristateToNative('yes'), PD_TRUE);
  // Anything the ABI did not promise reads as unknown, never as a healthy `no`.
  assert.equal(tristateFromNative(7), 'unknown');
});

test('the job outcome mirror is exactly three members', () => {
  assert.deepEqual(Object.keys(JobOutcome), ['done', 'failed', 'unknown']);
});
