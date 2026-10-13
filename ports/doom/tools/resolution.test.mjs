import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';
import { fileURLToPath } from 'node:url';
import { sampleReference } from './benchmark.mjs';

const port = fileURLToPath(new URL('../', import.meta.url));
function command(executable, args) {
  const result = spawnSync(executable, args, { cwd: port, encoding: 'utf8', timeout: 10000 });
  assert.ifError(result.error);
  return result;
}

test('reference sampling selects the exact top-left pixel, including last rows and columns', () => {
  const raw = Buffer.alloc(320 * 200 * 2);
  for (let i = 0; i < 64000; i++) raw.writeUInt16LE(i, i * 2);
  assert.equal(sampleReference(raw, 1), raw);
  for (const scale of [2, 4]) {
    const sampled = sampleReference(raw, scale), width = 320 / scale, height = 200 / scale;
    assert.equal(sampled.length, width * height * 2);
    for (let y = 0; y < height; y++) for (let x = 0; x < width; x++)
      assert.equal(sampled.readUInt16LE((y * width + x) * 2), y * scale * 320 + x * scale);
  }
  assert.throws(() => sampleReference(raw, 3), /scale must/);
  assert.throws(() => sampleReference(raw.subarray(2), 2), /full 320x200/);
});

test('build configuration keys distinguish scales and reject unsupported values', () => {
  const base = ['tools/build-support.mjs', 'config-key', 'assets/doom1.wad', 'cooked', '64000', 'build/resolution-test', 'preflight'];
  const defaultKey = command(process.execPath, base);
  assert.equal(defaultKey.status, 0, defaultKey.stderr);
  const keys = [1, 2, 4].map(scale => {
    const result = command(process.execPath, [...base, String(scale)]);
    assert.equal(result.status, 0, result.stderr);
    return result.stdout.trim();
  });
  assert.equal(defaultKey.stdout.trim(), keys[0]);
  assert.equal(new Set(keys).size, 3);
  for (const scale of ['0', '3', '02', '2 4']) {
    const result = command(process.execPath, [...base, scale]);
    assert.equal(result.status, 1);
    assert.match(result.stderr, /resolution scale must be 1, 2 or 4/);
    const make = command('make', ['RESOLUTION_SCALE=' + scale, 'help']);
    assert.notEqual(make.status, 0);
    assert.match(make.stderr, /RESOLUTION_SCALE must be 1, 2 or 4/);
  }
});

test('launchers reject conflicting resolution flags before any build', () => {
  for (const launcher of ['run.sh', 'run-bf.sh']) {
    const help = command('bash', [launcher, '--help']);
    assert.equal(help.status, 0, help.stderr);
    assert.match(help.stdout, /--half-resolution/);
    assert.match(help.stdout, /--quarter-resolution/);
    for (const flags of [['--half-resolution', '--quarter-resolution'], ['--quarter-resolution', '--half-resolution']]) {
      const result = command('bash', [launcher, ...flags, '--build-only']);
      assert.equal(result.status, 2);
      assert.match(result.stderr, /resolution flags cannot be combined/);
    }
  }
});

test('a BF checkpoint cannot switch its compiled resolution', t => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'doom-resolution-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const checkpoint = path.join(directory, 'state'); fs.writeFileSync(checkpoint, 'fixture');
  for (const flag of ['--half-resolution', '--quarter-resolution']) {
    const result = command('bash', ['run-bf.sh', '--load-state', checkpoint, flag]);
    assert.equal(result.status, 2);
    assert.match(result.stderr, /resolution.*cannot be changed on resume/);
  }
});
