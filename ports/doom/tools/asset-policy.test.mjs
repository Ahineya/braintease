// Real Make regressions: validation policy must invalidate each executable,
// including when another program already rewrote the shared generated header.
// No Brainfuck or full-engine execution is involved.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';
import { cachedAssets } from './assets-pack.mjs';
import { BUILD, PORT, ROOT, RVM, WAD, build, digest, json, mkdir, prepareDisk, writeJSON } from './test-support.mjs';

test('test disk staging requires a current asset build validation certificate', () => {
  const parent = path.join(BUILD, 'tests', 'asset-policy');
  mkdir(parent);
  const directory = fs.mkdtempSync(path.join(parent, 'certificate-'));
  const asset = cachedAssets(WAD, path.join(directory, 'cooked.img'), 'cooked');
  const manifest = path.join(directory, 'manifest.json');
  const disk = path.join(directory, 'test.img');
  const config = metadata => ({ settings: { wad_mode: 'cooked' },
    wad: { sha256: digest(WAD) }, asset_image: metadata });
  writeJSON(manifest, config(asset));
  prepareDisk(WAD, disk, { wadMode: 'cooked', manifest });
  assert.equal(digest(disk), asset.sha256);
  const absent = { ...asset }; delete absent.validation;
  for (const metadata of [absent,
    { ...asset, validation: { ...asset.validation, validator_sha256: '0'.repeat(64) } },
    { ...asset, texture_count: asset.texture_count + 1 }]) {
    writeJSON(manifest, config(metadata));
    assert.throws(() => prepareDisk(WAD, disk, { wadMode: 'cooked', manifest }), /no current build validation/);
    assert.equal(digest(disk), asset.sha256, 'failed validation must preserve the destination');
  }
});

test('Make switches validation policies and skip-build rejects stale strict executables', { timeout: 300000 }, () => {
  const parent = path.join(BUILD, 'tests', 'asset-policy');
  mkdir(parent);
  const directory = fs.mkdtempSync(path.join(parent, 'run-'));
  const shared = path.join(directory, 'build');
  const binary = path.join(shared, 'ripple/texture_metadata/texture_metadata.bin');
  const manifest = path.join(shared, 'manifest.json');
  const key = file => fs.readFileSync(file + '.asset-config', 'utf8').trim();
  function compile(label, program, assetValidation, buildDir = shared, target = 'ripple') {
    const logs = path.join(directory, label);
    build(program, logs, { wad: WAD, wadMode: 'cooked', assetValidation, buildDir, target });
    return fs.readFileSync(path.join(logs, 'build-' + program + '-title.stdout.log'), 'utf8');
  }

  compile('initial-preflight', 'texture_metadata', 'preflight', shared, 'both');
  assert.equal(json(manifest).settings.asset_validation, 'preflight');
  const fastKey = key(binary), fastHash = digest(binary);
  const directSwitch = compile('same-program-guest', 'texture_metadata', 'guest');
  assert.match(directSwitch, /Ripple: tests\/texture_metadata\.c/);
  assert.equal(json(manifest).settings.asset_validation, 'guest');
  const strictKey = key(binary), strictHash = digest(binary);
  assert.notEqual(strictKey, fastKey, 'validation policy must enter the build key');
  assert.notEqual(strictHash, fastHash, 'guest policy must compile the strict loader');

  compile('other-program-preflight', 'wad_read_small', 'preflight');
  assert.equal(json(manifest).settings.asset_validation, 'preflight');
  assert.equal(key(binary), strictKey, 'another program must not bless the old executable');
  const sharedSwitch = compile('shared-header-preflight', 'texture_metadata', 'preflight');
  assert.match(sharedSwitch, /Ripple: tests\/texture_metadata\.c/);
  assert.equal(key(binary), fastKey);
  assert.notEqual(digest(binary), strictHash, 'shared-header switching must replace the strict executable');

  // Reproduce the dangerous --skip-build situation using actual builds:
  // a fast texture loader remains in the diagnostic directory while another
  // program updates its shared manifest/header to claim guest validation.
  const diagnosticBuild = path.join(shared, 'texture-metadata-guest-validation');
  compile('diagnostic-directory-fast-loader', 'texture_metadata', 'preflight', diagnosticBuild);
  compile('diagnostic-manifest-guest', 'wad_read_small', 'guest', diagnosticBuild);
  const output = path.join(directory, 'skip-build');
  const command = ['node', path.join(PORT, 'tools/texture-metadata-validate.mjs'),
    '--wad', WAD, '--output', output, '--skip-build'];
  const result = spawnSync(command[0], command.slice(1), {
    cwd: ROOT, env: { ...process.env, DOOM_BUILD: shared }, encoding: 'utf8', timeout: 30000,
  });
  fs.writeFileSync(path.join(directory, 'skip-build.stdout.log'), result.stdout || '');
  fs.writeFileSync(path.join(directory, 'skip-build.stderr.log'), result.stderr || '');
  assert.ifError(result.error);
  assert.equal(result.signal, null);
  assert.equal(result.status, 1);
  assert.match(result.stderr, /guest: executable has stale asset configuration/);
  const rejected = json(path.join(output, 'results.json'));
  assert.equal(rejected.success, false);
  assert.equal(rejected.checks.length, 0);
  assert.ok(rejected.commands.every(record => record.command[0] !== RVM),
    'stale validation policy must be rejected before any guest execution');
  writeJSON(path.join(directory, 'policy-results.json'), {
    success: true, fast_key: fastKey, strict_key: strictKey,
    fast_binary_sha256: fastHash, strict_binary_sha256: strictHash,
    skip_build_diagnostic: rejected.error,
  });
});
