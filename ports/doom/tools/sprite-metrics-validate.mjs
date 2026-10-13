#!/usr/bin/env node
// Exact old/native versus prepared/Ripple comparison, with bank-split writes.
// All guest executions are bounded Rust VM runs; this does not execute BF.
import fs from 'node:fs';
import path from 'node:path';
import { parseArgs } from 'node:util';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { validateSpriteMetrics } from './sprite-metrics.mjs';
import { wadDirectory } from './assets-pack.mjs';
import { BUILD, WAD, RVM, BANK, MEMORY, commands, build, digest, json, mkdir, prepareDisk,
  require, run, writeJSON } from './test-support.mjs';

export function syntheticSpriteMetricsWad(count = 6) {
  const origins = [-32768, -1, 0, 1, 12345, 32767];
  const lumps = [{ name: 'S_START', data: Buffer.alloc(0) }];
  for (let i = 0; i < count; ++i) {
    const width = i % 3 + 1, at = 8 + width * 4, data = Buffer.alloc(at + 6);
    data.writeInt16LE(width, 0); data.writeInt16LE(1, 2);
    data.writeInt16LE(origins[i % origins.length], 4);
    data.writeInt16LE(origins[(i + 2) % origins.length], 6);
    for (let x = 0; x < width; ++x) data.writeUInt32LE(at, 8 + x * 4);
    Buffer.from([0, 1, 0, 42, 0, 255]).copy(data, at);
    // Deliberately absent from sprnames: metric preparation must include all
    // namespace entries, not only sprites selected by R_InitSpriteDefs.
    lumps.push({ name: 'UN' + String(i).padStart(6, '0'), data });
  }
  lumps.push({ name: 'S_END', data: Buffer.alloc(0) });
  const directory = 12 + lumps.reduce((sum, lump) => sum + lump.data.length, 0);
  const source = Buffer.alloc(directory + lumps.length * 16);
  source.write('IWAD'); source.writeUInt32LE(lumps.length, 4); source.writeUInt32LE(directory, 8);
  let at = 12;
  lumps.forEach((lump, index) => {
    source.writeUInt32LE(at, directory + index * 16);
    source.writeUInt32LE(lump.data.length, directory + index * 16 + 4);
    source.write(lump.name, directory + index * 16 + 8, 8, 'latin1');
    lump.data.copy(source, at); at += lump.data.length;
  });
  return source;
}

export function validateMetrics({ wad = WAD, output, synthetic = false, large = false, empty = false } = {}) {
  const out = path.resolve(output || path.join(BUILD, 'tests/sprite-metrics'));
  mkdir(out);
  const source = path.join(out, 'original.wad');
  fs.writeFileSync(source, synthetic || large || empty
    ? syntheticSpriteMetricsWad(empty ? 0 : large ? 11001 : 6) : fs.readFileSync(wad));
  const result = { success: false, synthetic, large, empty, source_sha256: digest(source), commands, checks: [] };
  try {
    build('sprite_metrics', out, { wad: source, wadMode: 'cooked', target: 'both', assetValidation: 'preflight' });
    const strictBuild = path.join(BUILD, 'sprite-metrics-guest-validation');
    build('sprite_metrics', path.join(out, 'strict-build'), { wad: source, wadMode: 'cooked', target: 'ripple',
      assetValidation: 'guest', buildDir: strictBuild });
    const binary = path.join(BUILD, 'ripple/sprite_metrics/sprite_metrics.bin');
    const strictBinary = path.join(strictBuild, 'ripple/sprite_metrics/sprite_metrics.bin');
    const native = path.join(BUILD, 'native/sprite_metrics/sprite_metrics');
    const nativeOutput = run('native-original', [native], out, 60);
    const expected = nativeOutput.slice(nativeOutput.indexOf('SPRITE METRICS INITIALIZED'));
    require(expected.startsWith('SPRITE METRICS INITIALIZED') && expected.endsWith('SPRITE METRICS OK\n'),
      'native metric oracle failed');
    const disk = path.join(out, 'valid.img');
    const asset = prepareDisk(source, disk, { wadMode: 'cooked', manifest: path.join(BUILD, 'manifest.json') });
    const original = fs.readFileSync(disk), sourceBytes = fs.readFileSync(source), entries = wadDirectory(sourceBytes);
    const section = data => data.subarray(asset.sprite_metrics_word * 2,
      (asset.sprite_metrics_word + asset.sprite_metrics_words) * 2);
    result.summary = validateSpriteMetrics(section(original), sourceBytes, entries);
    result.asset = asset;
    const seconds = large ? 180 : 90;
    for (const [label, file] of [['preflight', binary], ['guest', strictBinary]]) {
      const reportPath = path.join(out, label + '.report.json');
      const output = run(label, [RVM, file, '--bank-size', BANK, '--memory', MEMORY, '--headless',
        '--clock', 'unthrottled', '--disk', disk, '--timeout', seconds, '--max-instructions', 1000000000,
        '--exit-code', '--report', reportPath], out, seconds + 10);
      const report = json(reportPath);
      require(report.success && report.stop_reason === 'halted', label + ': guest did not halt');
      require(output.slice(output.indexOf('SPRITE METRICS INITIALIZED')) === expected,
        label + ': metrics differ from native original WAD');
      result.checks.push({ label, success: true, report });
    }
    const payload = asset.sprite_metrics_word + 16, count = result.summary.count;
    const mutations = [
      { label: 'header', at: asset.sprite_metrics_word, value: 0, fast: true },
      { label: 'count', at: asset.sprite_metrics_word + 2, value: count + 1, fast: true },
      { label: 'padding', at: asset.sprite_metrics_word + 15, value: 1, fast: true },
      ...(count ? [
        { label: 'width-fraction', at: payload, value: 1 },
        { label: 'width-negative', at: payload + 1, value: 65535 },
        { label: 'left-fraction', at: payload + count * 2, value: 1 },
        { label: 'top-fraction', at: payload + count * 4, value: 1 },
      ] : []),
    ];
    for (const mutation of mutations) {
      const mutant = Buffer.from(original); mutant.writeUInt16LE(mutation.value, mutation.at * 2);
      let rejected = false;
      try { validateSpriteMetrics(section(mutant), sourceBytes, entries); }
      catch (error) { rejected = error.message.startsWith('Invalid sprite metrics:'); }
      require(rejected, mutation.label + ': offline validation accepted corruption');
      const mutantDisk = path.join(out, 'mutant.img'); fs.writeFileSync(mutantDisk, mutant);
      for (const [policy, file] of [['guest', strictBinary], ...(mutation.fast ? [['preflight', binary]] : [])]) {
        const label = 'corrupt-' + policy + '-' + mutation.label, reportPath = path.join(out, label + '.report.json');
        const execution = spawnSync(RVM, [file, '--bank-size', BANK, '--memory', MEMORY, '--headless',
          '--clock', 'unthrottled', '--disk', mutantDisk, '--timeout', 30, '--max-instructions', 100000000,
          '--exit-code', '--report', reportPath].map(String), { encoding: 'utf8', timeout: 40000 });
        fs.writeFileSync(path.join(out, label + '.stdout.log'), execution.stdout || '');
        fs.writeFileSync(path.join(out, label + '.stderr.log'), execution.stderr || '');
        require(!execution.error && execution.signal === null && execution.status !== 0,
          label + ': expected explicit guest failure');
        const report = json(reportPath);
        require(report.success && report.stop_reason === 'halted'
          && execution.stdout.includes('SPRITE METRICS ERROR')
          && !execution.stdout.includes('SPRITE METRICS INITIALIZED'), label + ': loader accepted corruption');
        result.checks.push({ label, success: true });
      }
    }
    require(digest(source) === result.source_sha256 && digest(disk) === asset.sha256, 'inputs changed');
    result.success = true;
    console.log('PASS SPRITE METRICS: ' + count + ' entries match native and both bank-split guest paths');
    return result;
  } catch (error) { result.error = error.message; throw error; }
  finally { writeJSON(path.join(out, 'results.json'), result); }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const { values } = parseArgs({ options: { wad: { type: 'string', default: WAD }, output: { type: 'string' },
      synthetic: { type: 'boolean' }, large: { type: 'boolean' }, empty: { type: 'boolean' } } });
    validateMetrics(values);
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
