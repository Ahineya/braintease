#!/usr/bin/env node
// Bounded RCC/Cranelift checks against the independent host-compiler build.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import assert from 'node:assert/strict';

const port = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const root = path.resolve(port, '../..');
const build = path.resolve(process.env.DOOM_BUILD || path.join(port, 'build'));
const out = path.join(build, 'native-validation');
fs.mkdirSync(out, { recursive: true });
function run(executable, args, label, env = {}) {
  const result = spawnSync(executable, args, {
    cwd: root, env: { ...process.env, ...env }, timeout: 300000,
    maxBuffer: 16 * 1024 * 1024, encoding: 'utf8',
  });
  fs.writeFileSync(path.join(out, `${label}.log`), `${result.stdout || ''}${result.stderr || ''}`);
  if (result.error || result.status !== 0)
    throw new Error(`${label}: ${result.error || `exit ${result.status}`}\n${result.stderr || ''}\nSee ${out}/${label}.log`);
  return result.stdout;
}
function make(target, map = 0, demo = 0) {
  run('make', ['-C', port, '-j4', `TARGET=${target}`, `BUILD=${build}`,
    `WAD=${process.env.DOOM_WAD || path.join(port, 'assets/doom1.wad')}`,
    `WAD_MODE=${process.env.DOOM_WAD_MODE || 'cooked'}`, 'PROGRAM=engine',
    'LOW_DETAIL=0', 'RESOLUTION_SCALE=1', `START_MAP=${map}`, `START_DEMO=${demo}`], `build-${target}-${map}-${demo}`);
}
function compare(label, scene, frame, replay) {
  let input;
  if (replay) {
    input = path.join(port, 'tests', replay);
    const events = JSON.parse(fs.readFileSync(input));
    fs.writeFileSync(path.join(out, `${label}.input.txt`), events.map(e => `${e.tick} ${e.key} ${Number(e.down)}\n`).join(''));
  }
  const reference = path.join(out, `${label}-clang.rgb565`);
  const actual = path.join(out, `${label}-rcc.rgb565`);
  fs.rmSync(reference, { force: true }); fs.rmSync(actual, { force: true });
  const expectedLog = run(path.join(build, 'native/engine/engine'),
    [reference, String(frame), scene, ...(input ? [path.join(out, `${label}.input.txt`)] : [])], `${label}-clang`);
  const actualLog = run(path.join(build, 'rcc-native/engine/engine'), [], `${label}-rcc`, {
    RCC_NATIVE_HEADLESS: '1', RCC_NATIVE_PACED: undefined, RCC_NATIVE_FRAME: actual, RCC_NATIVE_FRAME_NUMBER: String(frame), RCC_NATIVE_TIMEOUT: '60',
    RCC_NATIVE_INPUT_SCRIPT: input,
  });
  assert.equal(fs.statSync(actual).size, 320 * 200 * 2);
  assert.deepEqual(fs.readFileSync(actual), fs.readFileSync(reference), `${label}: RGB565 pixels`);
  const records = text => text.split('\n').filter(line => line.startsWith('ENGINE frame '));
  assert.equal(records(actualLog).length, frame);
  assert.deepEqual(records(actualLog), records(expectedLog), `${label}: logical frames`);
  console.log(`${label}: ${frame} frame records and final pixels match`);
}
try {
  make('native'); make('rcc-native');
  compare('title', 'title', 1);
  compare('menu', 'title', 1, 'menu-open.json');
  make('rcc-native', 1);
  compare('map', 'map', 1);
  compare('gameplay-32', 'map', 32, 'gameplay-long.json');
  compare('gameplay-180', 'map', 180, 'gameplay-long.json');
  make('rcc-native', 0, 1);
  compare('demo-32', 'demo1', 32);
  // Return the regular launcher to its default configuration.
  make('rcc-native');
  console.log(`Native Doom validation passed. Captures and logs: ${out}`);
} catch (error) { console.error(error.message); process.exitCode = 1; }
