#!/usr/bin/env node
// Compare every produced sprite cell with the original engine's native WAD
// name resolver. Rust VM executions are bounded; this never executes BF.
import fs from 'node:fs';
import path from 'node:path';
import { parseArgs } from 'node:util';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { readSpriteNames, cookSpriteMetadata, validateSpriteMetadata } from './sprite-metadata.mjs';
import { wadDirectory } from './assets-pack.mjs';
import { BUILD, WAD, RVM, BANK, MEMORY, commands, build, digest, json, mkdir, prepareDisk,
  require, run, writeJSON } from './test-support.mjs';

export function syntheticSpriteWad(empty = false, tiny = false) {
  const patch = Buffer.from([1, 0, 1, 0, 0, 0, 0, 0, 12, 0, 0, 0, 0, 1, 0, 42, 0, 255]);
  const lumps = [{ name: 'S_START', data: Buffer.alloc(0) },
    ...(empty ? [] : tiny ? [{ name: 'TROOA0', data: patch }] : readSpriteNames().flatMap(name => Array.from({ length: 29 }, (_, frame) =>
      ({ name: name + String.fromCharCode(65 + frame) + '0', data: patch })))),
    { name: 'S_END', data: Buffer.alloc(0) }];
  const directory = 12 + lumps.reduce((sum, lump) => sum + lump.data.length, 0);
  const data = Buffer.alloc(directory + lumps.length * 16);
  data.write('IWAD'); data.writeUInt32LE(lumps.length, 4); data.writeUInt32LE(directory, 8);
  let at = 12;
  lumps.forEach((lump, index) => {
    data.writeUInt32LE(at, directory + index * 16);
    data.writeUInt32LE(lump.data.length, directory + index * 16 + 4);
    data.write(lump.name, directory + index * 16 + 8, 8, 'latin1');
    lump.data.copy(data, at); at += lump.data.length;
  });
  return data;
}

export function validateSprites({ wad = WAD, output, synthetic = false, empty = false, tiny = false } = {}) {
  const out = path.resolve(output || path.join(BUILD, 'tests/sprite-metadata'));
  mkdir(out);
  const source = path.join(out, 'original.wad');
  fs.writeFileSync(source, synthetic || empty || tiny ? syntheticSpriteWad(empty, tiny) : fs.readFileSync(wad));
  const result = { success: false, synthetic, empty, tiny, source_sha256: digest(source), commands, checks: [] };
  try {
    build('sprite_metadata', out, { wad: source, wadMode: 'cooked', target: 'both', assetValidation: 'preflight' });
    const strictBuild = path.join(BUILD, 'sprite-metadata-guest-validation');
    build('sprite_metadata', path.join(out, 'strict-build'), { wad: source, wadMode: 'cooked', target: 'ripple',
      assetValidation: 'guest', buildDir: strictBuild });
    const binary = path.join(BUILD, 'ripple/sprite_metadata/sprite_metadata.bin');
    const strictBinary = path.join(strictBuild, 'ripple/sprite_metadata/sprite_metadata.bin');
    const native = path.join(BUILD, 'native/sprite_metadata/sprite_metadata');
    const nativeOutput = run('native-original', [native], out, 60);
    const expected = nativeOutput.slice(nativeOutput.indexOf('SPRITE METADATA INITIALIZED'));
    require(expected.startsWith('SPRITE METADATA INITIALIZED') && expected.includes('SPRITE METADATA OK '),
      'native sprite oracle failed');
    const disk = path.join(out, 'valid.img');
    const asset = prepareDisk(source, disk, { wadMode: 'cooked', manifest: path.join(BUILD, 'manifest.json') });
    const original = fs.readFileSync(disk), entries = wadDirectory(fs.readFileSync(source));
    const section = original.subarray(asset.sprite_word * 2, (asset.sprite_word + asset.sprite_words) * 2);
    validateSpriteMetadata(section, entries);
    result.asset = asset;
    result.expected = expected.match(/SPRITE METADATA OK .+/)?.[0];
    for (const [label, file] of [['preflight', binary], ['guest', strictBinary]]) {
      const reportPath = path.join(out, label + '.report.json');
      const output = run(label, [RVM, file, '--bank-size', BANK, '--memory', MEMORY, '--headless',
        '--clock', 'unthrottled', '--disk', disk, '--timeout', 60, '--max-instructions', 500000000,
        '--exit-code', '--report', reportPath], out, 70);
      const report = json(reportPath);
      require(report.success && report.stop_reason === 'halted', label + ': guest did not halt');
      require(output.slice(output.indexOf('SPRITE METADATA INITIALIZED')) === expected,
        label + ': sprite frames differ from native original WAD');
      result.checks.push({ label, success: true, report });
    }

    // Corrupt copied prepared data: ordinary loaders retain bounds checks,
    // diagnostic loaders additionally check frame mappings. These bypass the
    // launcher's artifact identity gate deliberately to exercise guest errors.
    const frameBase = asset.sprite_word + 16 + asset.sprite_count * 3;
    const mutations = [
      { label: 'header', at: asset.sprite_word, value: 0, fast: true },
      { label: 'frame-count', at: asset.sprite_word + 16, value: 30, fast: true },
      { label: 'pointer-placeholder', at: asset.sprite_word + 17, value: 1 },
      ...(asset.sprite_frames ? [
        { label: 'rotation-flag', at: frameBase, value: 2 },
        { label: 'lump-outside', at: frameBase + 1, value: 65535 },
        { label: 'flip-outside', at: frameBase + 9, value: 2 },
      ] : []),
    ];
    for (const mutation of mutations) {
      const mutant = Buffer.from(original);
      mutant.writeUInt16LE(mutation.value, mutation.at * 2);
      let rejected = false;
      try { validateSpriteMetadata(mutant.subarray(asset.sprite_word * 2,
        (asset.sprite_word + asset.sprite_words) * 2), entries); }
      catch (error) { rejected = error.message.startsWith('Invalid sprite metadata:'); }
      require(rejected, mutation.label + ': native readback accepted corruption');
      const mutantDisk = path.join(out, 'mutant.img'); fs.writeFileSync(mutantDisk, mutant);
      for (const [policy, file] of [['guest', strictBinary], ...(mutation.fast ? [['preflight', binary]] : [])]) {
        const label = 'corrupt-' + policy + '-' + mutation.label;
        const reportPath = path.join(out, label + '.report.json');
        const command = [file, '--bank-size', BANK, '--memory', MEMORY, '--headless', '--clock', 'unthrottled',
          '--disk', mutantDisk, '--timeout', 30, '--max-instructions', 100000000,
          '--exit-code', '--report', reportPath].map(String);
        const execution = spawnSync(RVM, command, { encoding: 'utf8', timeout: 40000 });
        fs.writeFileSync(path.join(out, label + '.stdout.log'), execution.stdout || '');
        fs.writeFileSync(path.join(out, label + '.stderr.log'), execution.stderr || '');
        require(!execution.error && execution.signal === null && execution.status !== 0,
          label + ': expected explicit guest failure');
        const report = json(reportPath);
        require(report.success && report.stop_reason === 'halted'
          && execution.stdout.includes('SPRITE METADATA ERROR')
          && !execution.stdout.includes('SPRITE METADATA INITIALIZED'), label + ': loader did not reject corruption');
        result.checks.push({ label, success: true });
      }
    }

    // Independently confirm source rejection against the actual native engine,
    // using only this test's private WAD copy and restoring it after each case.
    const sourceBytes = fs.readFileSync(source), directory = sourceBytes.readUInt32LE(8);
    const first = asset.sprite_first_lump, last = asset.sprite_last_lump;
    const selected = new Set(readSpriteNames());
    const candidate = entries.slice(first, last + 1).find(entry => selected.has(entry.name.slice(0, 4))
      && entry.nameBytes[5] === 48 && entry.nameBytes[6] === 0);
    const malformed = [];
    if (candidate && first < last) {
      const other = candidate.index === first ? first + 1 : first;
      const duplicate = Buffer.from(sourceBytes);
      candidate.nameBytes.copy(duplicate, directory + other * 16 + 8);
      malformed.push(['duplicate', duplicate]);
      const missingRotation = Buffer.from(sourceBytes);
      missingRotation[directory + candidate.index * 16 + 8 + 5] = 49;
      malformed.push(['missing-rotation', missingRotation]);
      const invalidFrame = Buffer.from(sourceBytes);
      invalidFrame[directory + candidate.index * 16 + 8 + 4] = 127;
      malformed.push(['frame-outside', invalidFrame]);
    }
    for (const [label, data] of malformed) {
      let rejected = false;
      try { cookSpriteMetadata(wadDirectory(data)); }
      catch (error) { rejected = error.message.startsWith('Invalid sprite metadata:'); }
      require(rejected, label + ': packer accepted invalid original names');
      let execution;
      try {
        fs.writeFileSync(source, data);
        execution = spawnSync(native, [], { encoding: 'utf8', timeout: 10000 });
      } finally { fs.writeFileSync(source, sourceBytes); }
      fs.writeFileSync(path.join(out, 'native-invalid-' + label + '.stdout.log'), execution.stdout || '');
      require(!execution.error && execution.status !== 0 && execution.signal === null
        && execution.stdout.includes('SPRITE METADATA ERROR')
        && !execution.stdout.includes('SPRITE METADATA INITIALIZED'), label + ': native original resolver accepted invalid names');
      result.checks.push({ label: 'native-invalid-' + label, success: true });
    }
    require(digest(source) === result.source_sha256, 'source WAD changed');
    require(digest(disk) === asset.sha256, 'asset image changed');
    result.success = true;
    console.log('PASS ' + result.expected + ': every frame cell matches native, preflight and strict guest paths');
    return result;
  } catch (error) { result.error = error.message; throw error; }
  finally { writeJSON(path.join(out, 'results.json'), result); }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const { values } = parseArgs({ options: { wad: { type: 'string', default: WAD },
      output: { type: 'string' }, synthetic: { type: 'boolean' }, empty: { type: 'boolean' }, tiny: { type: 'boolean' } } });
    validateSprites(values);
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
