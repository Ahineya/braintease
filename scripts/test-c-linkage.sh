#!/usr/bin/env bash
# Regression coverage for separate compilation and callbacks on the unchanged ISA.
# Build tools first (cargo build --release and the separate ripple-asm workspace).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export RCC="${RCC:-$ROOT/target/release/rcc}"
export RASM="${RASM:-$ROOT/src/ripple-asm/target/release/rasm}"
export RLINK="${RLINK:-$ROOT/src/ripple-asm/target/release/rlink}"
export RVM="${RVM:-$ROOT/target/release/rvm}"
export LINKAGE_ROOT="$ROOT"
"${NODE:-node}" --input-type=module <<'JS'
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const root = process.env.LINKAGE_ROOT;
const [rcc, rasm, rlink, rvm] = ['RCC', 'RASM', 'RLINK', 'RVM'].map(name => process.env[name]);

function command(argv, success = true) {
  const args = argv.map(String);
  const result = spawnSync(args[0], args.slice(1), { encoding: 'utf8', timeout: 30000 });
  if (result.error) throw result.error;
  assert.equal(result.signal, null, `Command interrupted: ${args.join(' ')}`);
  if (success) {
    assert.equal(result.status, 0, `${args.join(' ')}\n${result.stdout}\n${result.stderr}`);
  } else {
    assert.notEqual(result.status, 0, `Expected diagnostic: ${args.join(' ')}`);
  }
  return result;
}

const work = fs.mkdtempSync(path.join(os.tmpdir(), 'rcc-linkage-'));
try {
  const fixtures = path.join(root, 'c-test/fixtures/linkage');
  function compileC(source, name, bank) {
    const asm = path.join(work, `${name}.asm`);
    command([rcc, 'compile', source, '-o', asm, '--bank-size', bank]);
    const obj = path.join(work, `${name}.pobj`);
    command([rasm, 'assemble', asm, '-o', obj, '--bank-size', bank, '--memory-offset', 0]);
    return obj;
  }

  const startup = path.join(work, 'crt0.pobj');
  for (const bank of [64000, 1024]) {
    command([rasm, 'assemble', path.join(root, 'runtime/crt0.asm'), '-o', startup,
             '--bank-size', bank, '--memory-offset', 0]);
    const objects = ['main', 'first', 'second'].map(name => compileC(path.join(fixtures, `${name}.c`), name, bank));
    let padding = [];
    if (bank === 1024) {
      // Complete code banks between startup and C guarantee code pointers have
      // nonzero banks, including external relocations and callback tables.
      const padSource = path.join(work, 'padding.asm');
      fs.writeFileSync(padSource, 'code_padding:\n' + 'NOP\n'.repeat(bank));
      const padObject = path.join(work, 'padding.pobj');
      command([rasm, 'assemble', padSource, '-o', padObject, '--bank-size', bank]);
      padding = [padObject];
    }
    for (const order of [objects, [...objects].reverse()]) {
      const binary = path.join(work, 'test.bin');
      command([rlink, startup, ...padding, ...order, '-o', binary, '--bank-size', bank]);
      command([rvm, binary, '--bank-size', bank, '--exit-code']);
    }
    console.log(`PASS: globals, externs, private symbols, callbacks and tables (bank size ${bank}, both link orders)`);
  }

  const tooLarge = path.join(work, 'too_large.c');
  fs.writeFileSync(tooLarge, 'int data[1025]; int main(void) { return 0; }\n');
  let result = command([rcc, 'compile', tooLarge, '-o', path.join(work, 'too_large.asm'), '--bank-size', 1024], false);
  assert.match(result.stderr + result.stdout, /exceeds one data bank/);

  const large = path.join(work, 'large.c');
  fs.writeFileSync(large, 'int data_one[700];\n');
  const objOne = compileC(large, 'large_one', 1024);
  fs.writeFileSync(large, 'int data_two[700];\n');
  const objTwo = compileC(large, 'large_two', 1024);
  result = command([rlink, objOne, objTwo, '-o', path.join(work, 'overflow.bin'), '--bank-size', 1024], false);
  assert.match(result.stderr + result.stdout, /exceeds GP bank capacity/);

  const largeFrame = path.join(work, 'large_frame.c');
  for (const [localCount, bank] of [[1000, 1024], [32750, 64000]]) {
    fs.writeFileSync(largeFrame, `int main(void) { int local[${localCount}]; return 0; }\n`);
    result = command([rcc, 'compile', largeFrame, '-o', path.join(work, 'large_frame.asm'), '--bank-size', bank], false);
    assert.match(result.stderr + result.stdout, /exceeds stack frame capacity/);
  }

  const declaration = path.join(work, 'undefined.c');
  fs.writeFileSync(declaration, 'extern int missing; int main(void) { return missing; }\n');
  const obj = compileC(declaration, 'undefined', 1024);
  result = command([rlink, startup, obj, '-o', path.join(work, 'undefined.bin'), '--bank-size', 1024], false);
  assert.match(result.stderr + result.stdout, /Undefined data label: missing/);
  console.log('PASS: compiler/linker data and frame overflow, undefined extern diagnostics');
} finally {
  fs.rmSync(work, { recursive: true, force: true });
}
JS
