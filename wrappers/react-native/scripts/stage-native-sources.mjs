#!/usr/bin/env node
//
// Stage the SDK's C++ sources into native/ so the npm tarball is self-contained.
//
// WHY THIS EXISTS
//   Inside this git checkout the podspec and android/CMakeLists.txt compile
//   ../../core/src, ../../capi/src, ../../queue/src and ../../dsl/src IN PLACE -- the same
//   files CMakeLists.txt, Package.swift and wrappers/android compile, never a private copy
//   of the engine. That is the rule the repository holds every wrapper to, and it is why a
//   core fix cannot reach three platforms and miss the fourth.
//
//   An npm tarball cannot contain files above its own directory. So `npm pack` runs this
//   first (package.json "prepack") and drops a COPY under native/. The build files prefer
//   native/ when it exists and fall back to ../../ when it does not, so:
//
//     - a developer working in this repository builds the real sources, with no copy in
//       sight and nothing to keep in sync;
//     - an app that installed the package from npm builds the copy that was staged from
//       those same sources at publish time, by this script, in one step that is part of
//       packing rather than a thing somebody remembers to do.
//
//   native/ is git-ignored for the same reason: it is a build artifact, not a source of
//   truth, and a committed copy is exactly the drift this arrangement exists to prevent.
//
// USAGE
//   node scripts/stage-native-sources.mjs           # copy
//   node scripts/stage-native-sources.mjs --clean   # remove native/

import { cpSync, existsSync, mkdirSync, rmSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const packageRoot = resolve(here, '..');
const repoRoot = resolve(packageRoot, '../..');
const target = join(packageRoot, 'native');

/** Every directory the podspec and CMakeLists compile or include. */
const staged = [
  'core/include',
  'core/src',
  'capi/include',
  'capi/src',
  'queue/include',
  'queue/src',
  'dsl/include',
  'dsl/src',
];

if (process.argv.includes('--clean')) {
  rmSync(target, { recursive: true, force: true });
  console.log(`removed ${target}`);
  process.exit(0);
}

for (const relative of staged) {
  const source = join(repoRoot, relative);
  if (!existsSync(source)) {
    console.error(
      `stage-native-sources: ${source} does not exist. This script only works inside the ` +
        'PrinterDriver repository; a published tarball already carries native/.'
    );
    process.exit(1);
  }
}

rmSync(target, { recursive: true, force: true });
mkdirSync(target, { recursive: true });
for (const relative of staged) {
  const destination = join(target, relative);
  mkdirSync(dirname(destination), { recursive: true });
  cpSync(join(repoRoot, relative), destination, { recursive: true });
}

console.log(`staged ${staged.length} source directories into ${target}`);
