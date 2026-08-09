/**
 * The wrapper parity contract (docs/api.md §17), checked from inside the package.
 *
 * "Every public pd.h function is bound in every wrapper." The repository-wide gate is
 * scripts/check_parity.sh; this is the same question asked locally, which means a
 * developer working only in wrappers/react-native/ finds a missing binding without
 * running the whole repository's checks.
 *
 * Three layers are checked, because a name in a TypeScript array proves nothing on its
 * own:
 *   1. every pd_* function has a method name in the TurboModule spec's runtime list;
 *   2. the `Spec` interface really declares that method (source text, not types);
 *   3. cpp/PrinterDriverModule.cpp really calls that pd_* function.
 */

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { generatedFunctions } from '../src/generated/abi.generated.ts';
import { methodNameForAbiFunction, nativeMethodNames } from '../src/methodNames.ts';

const here = dirname(fileURLToPath(import.meta.url));
const packageRoot = resolve(here, '..');
const specSource = readFileSync(resolve(packageRoot, 'src/NativePrinterDriver.ts'), 'utf8');
const moduleSource = readFileSync(resolve(packageRoot, 'cpp/PrinterDriverModule.cpp'), 'utf8');

function stripComments(source: string): string {
  return source.replace(/\/\*[\s\S]*?\*\//g, ' ').replace(/\/\/[^\n]*/g, ' ');
}

const specBody = stripComments(specSource);
const moduleBody = stripComments(moduleSource);

test('the naming rule is the one the generator and the module both use', () => {
  assert.equal(methodNameForAbiFunction('pd_add_printer_tcp'), 'addPrinterTcp');
  assert.equal(methodNameForAbiFunction('pd_job_await'), 'jobAwait');
  assert.equal(methodNameForAbiFunction('pd_queue_is_blocked'), 'queueIsBlocked');
  assert.equal(methodNameForAbiFunction('pd_confidence_grade_letter'), 'confidenceGradeLetter');
  assert.equal(methodNameForAbiFunction('pd_print'), 'print');
});

test('pd.h yields the function list the rest of this file walks', () => {
  assert.ok(
    generatedFunctions.length >= 80,
    `expected at least 80 pd_* functions, saw ${generatedFunctions.length}`
  );
});

test('every public pd_* function has a TurboModule method name', () => {
  const declared = new Set(nativeMethodNames);
  const missing = generatedFunctions
    .map((name) => ({ abi: name, method: methodNameForAbiFunction(name) }))
    .filter((entry) => !declared.has(entry.method));
  assert.deepEqual(
    missing,
    [],
    'these pd.h functions have no method in src/NativePrinterDriver.ts'
  );
});

test('no method name is invented that pd.h does not back', () => {
  const expected = new Set(generatedFunctions.map(methodNameForAbiFunction));
  const extra = nativeMethodNames.filter((name) => !expected.has(name));
  assert.deepEqual(extra, [], 'nativeMethodNames lists methods with no pd_* behind them');
});

test('the Spec interface declares every method in the list', () => {
  const missing = nativeMethodNames.filter(
    (name) => !new RegExp(`\\n\\s*${name}\\s*\\(`).test(specBody)
  );
  assert.deepEqual(missing, [], 'declared in nativeMethodNames but absent from `interface Spec`');
});

test('the C++ module calls every public pd_* function', () => {
  // This is the layer that actually matters: an entry in a TypeScript array is a promise,
  // a `pd_x(` in the translation unit that the syntax check compiles is evidence.
  const missing = generatedFunctions.filter(
    (name) => !new RegExp(`\\b${name}\\s*\\(`).test(moduleBody)
  );
  assert.deepEqual(missing, [], 'these pd.h functions are never called by cpp/PrinterDriverModule.cpp');
});

test('the C++ method table spells the same names the JavaScript side calls', () => {
  const missing = nativeMethodNames.filter(
    (name) => !moduleBody.includes(`"${name}"`)
  );
  assert.deepEqual(missing, [], 'these method names are not registered in the C++ method table');
});

test('the generated mirror is in sync with pd.h', () => {
  // A cheap staleness check that does not shell out: the generator asserts these
  // invariants itself, so a stale artifact shows up as a count that no longer matches the
  // header. `npm run abi:check` is the full byte-for-byte comparison.
  const header = readFileSync(
    resolve(packageRoot, '../../capi/include/printerdriver/pd.h'),
    'utf8'
  );
  for (const name of generatedFunctions) {
    assert.ok(header.includes(name), `${name} is in the generated list but not in pd.h`);
  }
});
