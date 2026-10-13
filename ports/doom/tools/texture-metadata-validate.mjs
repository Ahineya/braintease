#!/usr/bin/env node
// Compare cooked texture initialization with the original-WAD native oracle,
// then exercise loader rejection before the oracle can inspect corrupt data.
// Every execution is bounded and uses the Rust VM; Brainfuck is never run.
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { parseArgs } from 'node:util';
import { fileURLToPath } from 'node:url';
import { assertSeparateFile } from './assets-pack.mjs';
import { ROOT, BUILD, WAD, RVM, BANK, MEMORY, commands, build, digest, hash,
  json, mkdir, prepareDisk, require, run, writeJSON } from './test-support.mjs';

// More than one bank of columns and several banks of offset cells. Mixed
// overlapping and direct columns, duplicate names, negative origins, and an
// empty post column all pass through the same renderer/oracle as the IWAD.
function syntheticWad() {
  function patch(width) {
    const base = 8 + width * 4, data = Buffer.alloc(base + width * 6 + 1);
    data.writeUInt16LE(width); data.writeUInt16LE(16, 2);
    for (let x = 0; x < width; ++x) {
      data.writeUInt32LE(base + x * 6, 8 + x * 4);
      data.set([0, 1, 0, x % 251, 0, 255], base + x * 6);
    }
    // A direct empty column points at the final terminator. The lookup's
    // pixel offset is therefore two cells beyond the allocation, but Doom
    // checks the terminator at offset-3 before following any pixel payload.
    data[data.length - 1] = 255;
    data.writeUInt32LE(data.length - 1, 8 + Math.min(width - 1, 1007) * 4);
    return data;
  }
  function textures(first) {
    const count = 40, start = 4 + count * 4, data = Buffer.alloc(start + count * 42);
    data.writeUInt32LE(count);
    for (let i = 0; i < count; ++i) {
      const at = start + i * 42, index = first + i;
      data.writeUInt32LE(at, 4 + i * 4);
      data.write('TEX' + String(index % 37).padStart(4, '0'), at, 8, 'ascii');
      data.writeUInt16LE(1000, at + 12); data.writeUInt16LE(index % 2 ? 16 : 256, at + 14);
      data.writeUInt16LE(2, at + 20);
      data.writeInt16LE(-8, at + 22); data.writeInt16LE(-3, at + 24);
      data.writeInt16LE(index % 61, at + 32); data.writeInt16LE(-2, at + 34);
      data.writeUInt16LE(1, at + 36);
    }
    return data;
  }
  const names = Buffer.alloc(20); names.writeUInt32LE(2); names.write('BASE', 4); names.write('OVERLAY', 12);
  const lumps = [{ name: 'PNAMES', data: names }, { name: 'TEXTURE1', data: textures(0) },
    { name: 'TEXTURE2', data: textures(40) }, { name: 'BASE', data: patch(1024) },
    { name: 'OVERLAY', data: patch(64) }];
  const directory = 12 + lumps.reduce((sum, lump) => sum + lump.data.length, 0);
  const result = Buffer.alloc(directory + lumps.length * 16);
  result.write('IWAD'); result.writeUInt32LE(lumps.length, 4); result.writeUInt32LE(directory, 8);
  let at = 12;
  lumps.forEach((lump, index) => {
    const record = directory + index * 16;
    result.writeUInt32LE(at, record); result.writeUInt32LE(lump.data.length, record + 4);
    result.write(lump.name, record + 8, 8, 'ascii'); lump.data.copy(result, at); at += lump.data.length;
  });
  return result;
}

export function validateTextureMetadata({ wad = WAD, output, timeout = 60,
  maxInstructions = 500000000, skipBuild = false, synthetic = false } = {}) {
  require(Number.isSafeInteger(timeout) && timeout > 0, 'timeout must be a positive integer');
  require(Number.isSafeInteger(maxInstructions) && maxInstructions > 0, 'instruction limit must be a positive integer');
  const out = path.resolve(output || path.join(BUILD, 'tests', 'texture-metadata-validation'));
  mkdir(out);
  if (synthetic) {
    wad = path.join(out, 'synthetic-textures.wad');
    fs.writeFileSync(wad, syntheticWad());
  }
  const source = fs.realpathSync(wad), sourceHash = digest(source);
  const result = { format_version: 1, success: false, synthetic,
    source: { path: source, sha256: sourceHash }, wad_mode: 'cooked',
    bank_size: BANK, memory_cells: MEMORY, limits: { seconds: timeout, instructions: maxInstructions },
    checks: [], commands };
  try {
    if (!skipBuild) build('texture_metadata', out, { wad: source, wadMode: 'cooked', target: 'both' });
    const binary = path.join(BUILD, 'ripple/texture_metadata/texture_metadata.bin');
    const native = path.join(BUILD, 'native/texture_metadata/texture_metadata');
    const binaryHash = digest(binary), nativeHash = digest(native);
    const nativeOutput = run('native-original-wad', [native], out, timeout);
    const expected = nativeOutput.match(/^TEXTURE METADATA OK .+$/m)?.[0];
    require(expected && nativeOutput.includes('TEXTURE METADATA INITIALIZED'), 'native original-WAD oracle failed');
    const disk = path.join(out, 'valid.img');
    const asset = prepareDisk(source, disk, { wadMode: 'cooked', manifest: path.join(BUILD, 'manifest.json') });
    require(asset.format_version === 3 && asset.texture_words > 16 && asset.texture_count > 0,
      'texture validation requires cooked v3 textures');
    const original = fs.readFileSync(disk);
    const working = assertSeparateFile(path.join(out, 'mutant.img'), [source, asset.prepared_path, disk, binary, native]);
    result.binary = { path: binary, sha256: binaryHash };
    result.native = { path: native, sha256: nativeHash, expected };
    result.asset_image = asset;

    function runCase(label, image, corrupted = false, mutations = []) {
      const selectedDisk = corrupted ? working : disk;
      if (corrupted) fs.writeFileSync(working, image);
      const imageHash = hash(image), reportPath = path.join(out, label + '.report.json');
      const stdoutPath = path.join(out, label + '.stdout.log'), stderrPath = path.join(out, label + '.stderr.log');
      fs.rmSync(reportPath, { force: true });
      const command = [RVM, binary, '--bank-size', BANK, '--memory', MEMORY,
        '--headless', '--clock', 'unthrottled', '--seed', 1, '--disk', selectedDisk,
        '--timeout', timeout, '--max-instructions', maxInstructions,
        '--exit-code', '--report', reportPath].map(String);
      const record = { label, command, stdout: stdoutPath, stderr: stderrPath };
      commands.push(record);
      const handles = [fs.openSync(stdoutPath, 'w'), fs.openSync(stderrPath, 'w')];
      const start = performance.now();
      let execution;
      try {
        execution = spawnSync(command[0], command.slice(1), { cwd: ROOT, stdio: ['ignore', ...handles],
          timeout: (timeout + 10) * 1000, killSignal: 'SIGKILL' });
      } finally {
        handles.forEach(handle => fs.closeSync(handle));
        record.seconds = (performance.now() - start) / 1000;
      }
      record.exit_code = execution.status; record.signal = execution.signal;
      const check = { label, corrupted, mutations, disk_sha256: imageHash,
        exit_code: execution.status, success: false };
      result.checks.push(check);
      require(!execution.error && execution.signal === null, label + ': host execution failed or timed out');
      require(fs.existsSync(reportPath), label + ': missing VM report');
      const report = json(reportPath); check.vm_report = report;
      require(report.success === true && report.stop_reason === 'halted',
        label + ': expected explicit guest exit, got ' + report.stop_reason);
      require(report.binary_sha256 === binaryHash && report.disk_sha256 === imageHash
        && report.bank_size === BANK && report.memory_cells === MEMORY, label + ': incorrect execution inputs');
      const stdout = fs.readFileSync(stdoutPath, 'utf8');
      if (corrupted) {
        require(Number.isInteger(execution.status) && execution.status !== 0, label + ': corrupt input returned success');
        const diagnostic = stdout.split('\n').find(line => line.startsWith('TEXTURE METADATA ERROR '));
        require(diagnostic && !stdout.includes('TEXTURE METADATA INITIALIZED')
          && !stdout.includes('TEXTURE METADATA OK'), label + ': not rejected by loader; see ' + stdoutPath);
        check.diagnostic = diagnostic;
      } else {
        require(execution.status === 0 && stdout.trimEnd().endsWith(expected)
          && stdout.includes('TEXTURE METADATA INITIALIZED') && !stdout.includes('TEXTURE METADATA ERROR'),
        'cooked output differs from native original-WAD oracle; see ' + stdoutPath);
      }
      require(digest(selectedDisk) === imageHash && digest(source) === sourceHash
        && digest(asset.prepared_path) === asset.sha256 && digest(binary) === binaryHash
        && digest(native) === nativeHash, label + ': an execution input changed');
      check.success = true;
    }

    function mutate(label, changes) {
      const image = Buffer.from(original);
      const mutations = changes.map(([word, value]) => {
        require(Number.isSafeInteger(word) && word >= 0 && word < asset.total_words, label + ': invalid mutation address');
        require(Number.isInteger(value) && value >= 0 && value <= 65535, label + ': invalid mutation cell');
        const before = image.readUInt16LE(word * 2); image.writeUInt16LE(value, word * 2);
        return { word, before, after: value };
      });
      require(!image.equals(original), label + ': mutation did not change image');
      runCase(label, image, true, mutations);
    }
    const u32 = (word, value) => [[word, value & 65535], [word + 1, value >>> 16]];
    const header = asset.texture_word, definition = header + 16;
    const lumpBase = definition + asset.texture_definition_words;
    const offsetBase = lumpBase + asset.texture_columns;
    const maskBase = offsetBase + asset.texture_columns * 2;
    const heightBase = maskBase + asset.texture_count;
    const sizeBase = heightBase + asset.texture_count * 2;
    const headBase = sizeBase + asset.texture_count * 2;
    runCase('valid', original);
    console.log('PASS texture metadata: ' + expected + ' (native original WAD and cooked Rust VM)');
    for (let i = 0; i < 16; ++i) {
      const at = header + i;
      mutate('header-' + String(i).padStart(2, '0'), [[at, (original.readUInt16LE(at * 2) + 1) & 65535]]);
    }
    for (const [label, offset, value] of [
      ['name-high', 0, 256], ['width-zero', 8, 0], ['width-negative', 8, 32768],
      ['height-zero', 9, 0], ['height-negative', 9, 32768], ['index', 10, 1],
      ['next-self', 11, 0], ['next-outside', 11, asset.texture_count], ['next-high', 12, 1],
      ['patchcount-zero', 13, 0], ['patchcount-negative', 13, 32768], ['patch-id', 16, asset.lump_count],
    ]) mutate('definition-' + label, [[definition + offset, value]]);
    mutate('column-lump-outside', [[lumpBase, asset.lump_count]]);
    mutate('column-lump-negative', [[lumpBase, 65534]]);
    mutate('column-offset-outside', u32(offsetBase, 0xffffffff));
    mutate('width-mask', [[maskBase, 32768]]);
    mutate('fixed-height', [[heightBase, 1]]);
    mutate('composite-size', u32(sizeBase, 65537));
    mutate('hash-head-outside', [[headBase, asset.texture_count]]);
    mutate('hash-head-negative', [[headBase, 65534]]);
    let compositeColumn = -1;
    for (let i = 0; i < asset.texture_columns; ++i)
      if (original.readUInt16LE((lumpBase + i) * 2) === 65535) { compositeColumn = i; break; }
    if (compositeColumn >= 0) mutate('composite-column-offset', u32(offsetBase + compositeColumn * 2, 65536));
    require(digest(disk) === asset.sha256, 'valid reference image changed');
    result.success = true;
    console.log('PASS texture metadata: ' + (result.checks.length - 1)
      + ' corruptions rejected before initialization completed; source WAD and cache unchanged');
    return result;
  } catch (error) {
    result.error = error.message;
    throw error;
  } finally {
    writeJSON(path.join(out, 'results.json'), result);
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const { values } = parseArgs({ options: {
      wad: { type: 'string', default: WAD }, output: { type: 'string' },
      timeout: { type: 'string', default: '60' },
      'max-instructions': { type: 'string', default: '500000000' },
      'skip-build': { type: 'boolean', default: false }, synthetic: { type: 'boolean', default: false },
      help: { type: 'boolean' },
    } });
    if (values.help) console.log('Usage: node tools/texture-metadata-validate.mjs [--wad FILE] [--output DIRECTORY]\n'
      + '  [--timeout SECONDS] [--max-instructions N] [--skip-build] [--synthetic]\n'
      + 'Compares native and cooked renderer texture initialization, then rejects corrupted copied images.\n'
      + '--synthetic uses 80 textures and 80,000 columns to cross multiple memory banks.\n'
      + 'DOOM_BUILD selects an isolated build directory. All runs are bounded; no Brainfuck is executed.');
    else validateTextureMetadata({ wad: values.wad, output: values.output,
      timeout: Number(values.timeout), maxInstructions: Number(values['max-instructions']),
      skipBuild: values['skip-build'], synthetic: values.synthetic });
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
