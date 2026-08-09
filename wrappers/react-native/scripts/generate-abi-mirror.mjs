#!/usr/bin/env node
//
// Regenerate src/generated/abi.generated.ts from the real C ABI header,
// ../../capi/include/printerdriver/pd.h.
//
// WHY THIS EXISTS
//   The TypeScript enum mirrors in src/enums.ts are hand-written, because a hand-written
//   const object is what gives an app a closed string-literal union and a readable
//   autocomplete list. Hand-written mirrors drift. So the header is also parsed
//   mechanically into a checked-in artifact, and test/enums.test.ts asserts the two agree
//   member-for-member and value-for-value. A value added to pd.h without touching
//   src/enums.ts then fails `npm run abi:check` (the artifact is stale) or `npm test`
//   (the artifact and the mirror disagree) -- never silently becomes
//   "not implemented on platform X" (docs/api.md §1.3, §17).
//
//   The function list is generated for the same reason, one level up: docs/api.md §17
//   requires every public pd_* function to be reachable from every wrapper, and
//   test/parity.test.ts checks this package's native-module surface against it. The
//   repository-wide scripts/check_parity.sh is the enforcing gate; this is the local
//   copy of the same question, answerable without leaving the package.
//
// USAGE
//   node scripts/generate-abi-mirror.mjs           # write src/generated/abi.generated.ts
//   node scripts/generate-abi-mirror.mjs --check   # exit 1 if the checked-in file is stale
//
// The generator is deliberately dumb: no compiler, no libclang, just the two shapes pd.h
// actually uses (`typedef enum pd_x { ... } pd_x;` and one-line function declarations
// starting at column 0). If pd.h ever adopts a third shape, this fails loudly rather than
// quietly under-reporting -- see the sanity assertions at the bottom.

import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const packageRoot = resolve(here, '..');
const headerPath = resolve(packageRoot, '../../capi/include/printerdriver/pd.h');
const outputPath = resolve(packageRoot, 'src/generated/abi.generated.ts');

/** Strip `/* ... *\/` and `// ...` so neither can be mistaken for a declaration. */
function stripComments(source) {
  return source.replace(/\/\*[\s\S]*?\*\//g, ' ').replace(/\/\/[^\n]*/g, ' ');
}

/** `PD_JOB_STATE_QUEUED` + prefix `PD_JOB_STATE_` -> `queued`. */
export function camelKey(memberName, prefix) {
  const bare = memberName.startsWith(prefix) ? memberName.slice(prefix.length) : memberName;
  const parts = bare.toLowerCase().split('_').filter((part) => part.length > 0);
  return parts
    .map((part, index) => (index === 0 ? part : part[0].toUpperCase() + part.slice(1)))
    .join('');
}

/**
 * The longest common prefix of the value members, trimmed back to the last underscore.
 * pd.h never uses a bare prefix for one enum and a decorated one for another, so this is
 * exact -- and when it is not, the `_COUNT` sanity check below catches it.
 */
export function commonPrefix(names) {
  if (names.length === 0) return '';
  let prefix = names[0];
  for (const name of names.slice(1)) {
    let index = 0;
    while (index < prefix.length && index < name.length && prefix[index] === name[index]) index += 1;
    prefix = prefix.slice(0, index);
  }
  const cut = prefix.lastIndexOf('_');
  return cut < 0 ? '' : prefix.slice(0, cut + 1);
}

function parseEnums(source) {
  const enums = [];
  const pattern = /typedef\s+enum\s+(pd_[a-z0-9_]+)\s*\{([^}]*)\}\s*\1\s*;/g;
  let match;
  while ((match = pattern.exec(source)) !== null) {
    const [, name, body] = match;
    const members = [];
    for (const raw of body.split(',')) {
      const entry = raw.trim();
      if (entry.length === 0) continue;
      const memberMatch = /^([A-Z][A-Z0-9_]*)\s*=\s*(-?\d+)$/.exec(entry);
      if (memberMatch === null) {
        throw new Error(`${name}: cannot parse enum member ${JSON.stringify(entry)}`);
      }
      members.push({ name: memberMatch[1], value: Number(memberMatch[2]) });
    }
    const countMember = members.find((member) => member.name.endsWith('_COUNT'));
    const valueMembers = members.filter((member) => !member.name.endsWith('_COUNT'));
    if (countMember === undefined) {
      throw new Error(`${name}: no _COUNT member; pd.h promises every enum carries one`);
    }
    const prefix = commonPrefix(valueMembers.map((member) => member.name));
    enums.push({
      name,
      prefix,
      countName: countMember.name,
      count: countMember.value,
      members: valueMembers.map((member) => ({
        name: member.name,
        key: camelKey(member.name, prefix),
        value: member.value,
      })),
    });
  }
  return enums;
}

/**
 * Public function declarations. In pd.h every one of them starts at column 0 with its
 * return type and carries `name(` on that first line; struct fields and enum members are
 * indented, and every function-pointer alias starts with `typedef`.
 */
function parseFunctions(source) {
  const names = [];
  for (const line of source.split('\n')) {
    if (/^[\s#}{]/.test(line) || line.length === 0) continue;
    if (/^(typedef|extern|using|namespace)\b/.test(line)) continue;
    const match = /^[A-Za-z_][^()]*?\b(pd_[a-z0-9_]+)\s*\(/.exec(line);
    if (match === null) continue;
    names.push(match[1]);
  }
  return [...new Set(names)].sort();
}

const header = readFileSync(headerPath, 'utf8');
const stripped = stripComments(header);
const enums = parseEnums(stripped);
const functions = parseFunctions(stripped);

// --- Sanity assertions: a generator that silently under-reports is worse than none -----
if (enums.length < 20) {
  throw new Error(`only ${enums.length} enums parsed out of pd.h; the parser has drifted`);
}
if (functions.length < 80) {
  throw new Error(`only ${functions.length} functions parsed out of pd.h; the parser has drifted`);
}
for (const item of enums) {
  // pd_code_page is documented as the one enum whose _COUNT is a member count rather than
  // a past-the-end sentinel; both readings coincide with "number of value members", which
  // is what this asserts, so no exception is needed here.
  if (item.count !== item.members.length) {
    throw new Error(
      `${item.name}: ${item.countName} is ${item.count} but ${item.members.length} value members were parsed`
    );
  }
  const keys = new Set(item.members.map((member) => member.key));
  if (keys.size !== item.members.length) {
    throw new Error(`${item.name}: two members collapse onto the same camelCase key`);
  }
}

const banner = `// GENERATED FILE -- DO NOT EDIT.
//
// Produced by scripts/generate-abi-mirror.mjs from
// capi/include/printerdriver/pd.h. Regenerate with \`npm run abi:generate\`; verify with
// \`npm run abi:check\`. Its only consumers are the tests, which compare it against the
// hand-written mirrors in src/enums.ts and against the native module's method table.
//
// Enums: ${enums.length}. Public pd_* functions: ${functions.length}.
`;

const enumLines = enums
  .map((item) => {
    const members = item.members
      .map(
        (member) =>
          `      { name: '${member.name}', key: '${member.key}', value: ${member.value} },`
      )
      .join('\n');
    return `  {
    name: '${item.name}',
    prefix: '${item.prefix}',
    countName: '${item.countName}',
    count: ${item.count},
    members: [
${members}
    ],
  },`;
  })
  .join('\n');

const output = `${banner}
export interface GeneratedEnumMember {
  readonly name: string;
  readonly key: string;
  readonly value: number;
}

export interface GeneratedEnum {
  readonly name: string;
  readonly prefix: string;
  readonly countName: string;
  readonly count: number;
  readonly members: readonly GeneratedEnumMember[];
}

export const generatedEnums: readonly GeneratedEnum[] = [
${enumLines}
];

export const generatedFunctions: readonly string[] = [
${functions.map((name) => `  '${name}',`).join('\n')}
];
`;

if (process.argv.includes('--check')) {
  let current = '';
  try {
    current = readFileSync(outputPath, 'utf8');
  } catch {
    console.error(`abi:check FAILED -- ${outputPath} does not exist. Run npm run abi:generate.`);
    process.exit(1);
  }
  if (current !== output) {
    console.error(
      'abi:check FAILED -- src/generated/abi.generated.ts is stale against pd.h. ' +
        'Run npm run abi:generate and update src/enums.ts to match.'
    );
    process.exit(1);
  }
  console.log(`abi:check ok -- ${enums.length} enums, ${functions.length} functions, in sync with pd.h`);
  process.exit(0);
}

writeFileSync(outputPath, output);
console.log(`wrote ${outputPath}: ${enums.length} enums, ${functions.length} functions`);
