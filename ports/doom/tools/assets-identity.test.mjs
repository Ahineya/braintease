import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { ASSET_INPUTS, VALIDATION_INPUTS } from './assets-identity.mjs';

test('prepared asset identities include engine data, packer modules and validation modules', async t => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'doom-asset-identity-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const tools = path.join(directory, 'tools');
  fs.mkdirSync(tools);
  for (const file of VALIDATION_INPUTS) {
    const target = path.resolve(tools, file);
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.copyFileSync(new URL(file, import.meta.url), target);
  }
  const { assetGeneratorHash, assetValidatorHash } = await import(pathToFileURL(path.join(tools, 'assets-identity.mjs')));
  const generator = assetGeneratorHash(), validator = assetValidatorHash();
  for (const file of ['../engine/info.c', '../engine/tables.c', '../engine/r_main.h',
    '../engine/m_fixed.c', '../src/fixed_fast.asm',
    'sprite-metadata.mjs', 'sprite-metrics.mjs', '../engine/r_data.c',
    'renderer-tables.mjs', 'renderer-tables-validate.mjs', 'assets-validate.mjs']) {
    const target = path.resolve(tools, file), before = fs.readFileSync(target);
    fs.appendFileSync(target, '\n/* changed build input */\n');
    assert.notEqual(assetValidatorHash(), validator, file + ': validation must become stale');
    if (ASSET_INPUTS.includes(file)) assert.notEqual(assetGeneratorHash(), generator, file + ': cache must be regenerated');
    else assert.equal(assetGeneratorHash(), generator);
    fs.writeFileSync(target, before);
  }
  assert.equal(assetGeneratorHash(), generator);
  assert.equal(assetValidatorHash(), validator);
});
