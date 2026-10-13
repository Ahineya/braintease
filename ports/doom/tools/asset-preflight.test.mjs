// Exercise the boundary between prepared bytes and the ordinary launch path.
// These tests never execute Doom or change the source WAD.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const port = fileURLToPath(new URL('../', import.meta.url));
const source = path.join(port, 'assets/doom1.wad');
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const readJSON = file => JSON.parse(fs.readFileSync(file, 'utf8'));
const writeJSON = (file, value) => fs.writeFileSync(file, JSON.stringify(value));
function temporary(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'doom-preflight-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return directory;
}
function support(...args) {
  return spawnSync(process.execPath, [path.join(port, 'tools/build-support.mjs'), ...args],
    { encoding: 'utf8', timeout: 30000 });
}
function config(directory, policy = 'preflight') {
  return support('config', source, path.join(directory, 'generated'),
    path.join(directory, 'manifest.json'), '64000', 'engine', '0', '0', '', '',
    'clang', '', '', '', 'cooked', policy);
}

test('configuration and staging reuse the exact artifact checked by the packer', t => {
  const directory = temporary(t), originalHash = sha(fs.readFileSync(source));
  let result = config(directory);
  assert.equal(result.status, 0, result.stderr);
  const build = readJSON(path.join(directory, 'manifest.json'));
  assert.equal(build.settings.asset_validation, 'preflight');
  assert.equal(build.asset_preflight.image_sha256, build.asset_image.sha256);
  assert.equal(build.asset_preflight.validator_sha256,
    sha(fs.readFileSync(new URL('./assets-validate.mjs', import.meta.url))));
  assert.equal(build.asset_preflight.textures, 125);
  const modified = fs.statSync(build.asset_image.path).mtimeMs;
  result = config(directory);
  assert.equal(result.status, 0, result.stderr);
  assert.equal(fs.statSync(build.asset_image.path).mtimeMs, modified, 'valid cache is reused');
  const disk = path.join(directory, 'run.img');
  result = support('disk', source, disk, 'cooked', path.join(directory, 'manifest.json'));
  assert.equal(result.status, 0, result.stderr);
  const staged = JSON.parse(result.stdout);
  assert.equal(staged.asset_preflight.image_sha256, sha(fs.readFileSync(disk)));
  assert.deepEqual(staged.asset_preflight, build.asset_preflight);
  assert.equal(sha(fs.readFileSync(source)), originalHash);
});

test('changed image hashes cannot reuse a build validation record; rebuilding repairs the cache', t => {
  const directory = temporary(t), manifest = path.join(directory, 'manifest.json');
  assert.equal(config(directory).status, 0);
  const build = readJSON(manifest), asset = build.asset_image;
  const originalHash = asset.sha256;
  const disk = path.join(directory, 'run.img'), sentinel = Buffer.from('previous staged image');
  fs.writeFileSync(disk, sentinel);
  const bytes = fs.readFileSync(asset.path);
  // A legal 16-bit value but an impossible name cell in the prepared texture.
  bytes.writeUInt16LE(256, (asset.texture_word + 16) * 2);
  asset.sha256 = sha(bytes);
  const sidecar = readJSON(asset.path + '.json');
  sidecar.sha256 = asset.sha256;

  for (const policy of ['preflight', 'guest']) {
    fs.writeFileSync(asset.path, bytes);
    writeJSON(asset.path + '.json', sidecar);
    build.settings.asset_validation = policy;
    writeJSON(manifest, build);
    const staged = support('disk', source, disk, 'cooked', manifest);
    assert.notEqual(staged.status, 0);
    assert.match(staged.stderr, /no current build validation/);
    assert.deepEqual(fs.readFileSync(disk), sentinel, 'rejection happens before staging');
    const configured = config(directory, policy);
    assert.equal(configured.status, 0, configured.stderr);
    assert.equal(sha(fs.readFileSync(asset.path)), originalHash, 'changed cache is repacked and checked');
    const repaired = readJSON(manifest);
    assert.equal(repaired.asset_preflight.image_sha256, originalHash);
  }
});

test('artifacts without current validation are regenerated before use', t => {
  const directory = temporary(t), manifest = path.join(directory, 'manifest.json');
  assert.equal(config(directory).status, 0);
  const build = readJSON(manifest), sidecar = readJSON(build.asset_image.path + '.json');
  for (const invalid of [null, { ...sidecar.validation, validator_sha256: 'old validator' }]) {
    const altered = { ...sidecar, validation: invalid };
    writeJSON(build.asset_image.path + '.json', altered);
    writeJSON(manifest, { ...build, asset_image: altered });
    const staged = support('disk', source, path.join(directory, 'run.img'), 'cooked', manifest);
    assert.notEqual(staged.status, 0);
    assert.match(staged.stderr, /no current build validation/);
    assert.equal(config(directory).status, 0);
    assert.deepEqual(readJSON(build.asset_image.path + '.json').validation, sidecar.validation);
  }
});

test('staging rejects bytes changed after the build before overwriting the run image', t => {
  const directory = temporary(t), manifest = path.join(directory, 'manifest.json');
  assert.equal(config(directory).status, 0);
  const build = readJSON(manifest), image = fs.readFileSync(build.asset_image.path);
  image[0] ^= 1;
  fs.writeFileSync(build.asset_image.path, image);
  const disk = path.join(directory, 'run.img'), sentinel = Buffer.from('keep me');
  fs.writeFileSync(disk, sentinel);
  const result = support('disk', source, disk, 'cooked', manifest);
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /differs from build manifest/);
  assert.deepEqual(fs.readFileSync(disk), sentinel);
});

test('validation policy participates in build identity and rejects unknown settings', t => {
  const directory = temporary(t), generated = path.join(directory, 'generated');
  const key = policy => support('config-key', source, 'cooked', '64000', generated, policy);
  const fast = key('preflight'), strict = key('guest');
  assert.equal(fast.status, 0, fast.stderr);
  assert.equal(strict.status, 0, strict.stderr);
  assert.notEqual(fast.stdout, strict.stdout);
  assert.equal(key('preflight').stdout, fast.stdout);
  assert.notEqual(key('unchecked').status, 0);
  assert.notEqual(config(directory, 'unchecked').status, 0);
});
