#!/usr/bin/env node
// Exercise the guest's cooked-metadata checks with an unchanged linked
// wad_lookup fixture. Every run is bounded; no Brainfuck execution is used.
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { parseArgs } from 'node:util';
import { fileURLToPath } from 'node:url';
import { assertSeparateFile } from './assets-pack.mjs';
import { ROOT, BUILD, WAD, RVM, BANK, MEMORY, commands, build, digest, hash,
  json, mkdir, prepareDisk, require, writeJSON } from './test-support.mjs';

const EXPECTED = 'WAD LOOKUP OK 1264 lumps, 350 names, 598 references, final block\n';
const HEADER_WORDS = 32;

export function validateMetadata({ wad = WAD, output, timeout = 30,
  maxInstructions = 100000000, skipBuild = false } = {}) {
  require(Number.isSafeInteger(timeout) && timeout > 0, 'timeout must be a positive integer');
  require(Number.isSafeInteger(maxInstructions) && maxInstructions > 0,
    'instruction limit must be a positive integer');
  const source = fs.realpathSync(wad);
  const sourceHash = digest(source);
  const out = path.resolve(output || path.join(BUILD, 'tests', 'metadata-validation'));
  mkdir(out);
  const result = { format_version: 1, success: false,
    source: { path: source, sha256: sourceHash }, wad_mode: 'cooked',
    bank_size: BANK, memory_cells: MEMORY,
    limits: { seconds: timeout, instructions: maxInstructions }, checks: [], commands };
  try {
    if (!skipBuild) build('wad_lookup', out, { wad: source, wadMode: 'cooked', target: 'ripple' });
    const binary = path.join(BUILD, 'ripple', 'wad_lookup', 'wad_lookup.bin');
    const binaryHash = digest(binary);
    const disk = path.join(out, 'valid.img');
    const asset = prepareDisk(source, disk, { wadMode: 'cooked', manifest: path.join(BUILD, 'manifest.json') });
    require(asset.format_version === 3 && asset.directory_record_words === 18
      && asset.hash_entry_words === 1 && asset.pnames_entry_words === 1,
    'metadata validation requires the cooked v3 layout');
    require(asset.lump_count > 0 && asset.lump_count < 32767 && asset.pnames_count > 0,
      'wad_lookup metadata validation requires the shareware WAD with PNAMES');
    const original = fs.readFileSync(disk);
    const working = assertSeparateFile(path.join(out, 'mutant.img'),
      [source, asset.prepared_path, disk, binary]);
    result.binary = { path: binary, sha256: binaryHash };
    result.asset_image = asset;

    function runCase(label, image, expectedError, mutations = []) {
      const selectedDisk = expectedError ? working : disk;
      if (expectedError) fs.writeFileSync(working, image);
      const imageHash = hash(image);
      const reportPath = path.join(out, label + '.report.json');
      const stdoutPath = path.join(out, label + '.stdout.log');
      const stderrPath = path.join(out, label + '.stderr.log');
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
        execution = spawnSync(command[0], command.slice(1), {
          cwd: ROOT, stdio: ['ignore', ...handles],
          timeout: (timeout + 10) * 1000, killSignal: 'SIGKILL',
        });
      } finally {
        handles.forEach(handle => fs.closeSync(handle));
        record.seconds = (performance.now() - start) / 1000;
      }
      record.exit_code = execution.status;
      record.signal = execution.signal;
      const check = { label, expected_error: expectedError || null, mutations,
        disk_sha256: imageHash, exit_code: execution.status, success: false };
      result.checks.push(check);
      require(!execution.error && execution.signal === null,
        label + ': host execution failed or timed out; see ' + stderrPath);
      require(fs.existsSync(reportPath), label + ': missing VM report; see ' + stderrPath);
      const report = json(reportPath);
      check.vm_report = report;
      // I_Error exits through the normal guest halt path. A VM trap, timeout,
      // or exhausted instruction budget must never count as a rejection.
      require(report.success === true && report.stop_reason === 'halted',
        label + ': expected explicit guest exit, got ' + report.stop_reason);
      require(report.binary_sha256 === binaryHash && report.disk_sha256 === imageHash
        && report.bank_size === BANK && report.memory_cells === MEMORY,
      label + ': wrong binary, disk or memory configuration');
      const stdout = fs.readFileSync(stdoutPath, 'utf8');
      if (expectedError) {
        require(Number.isInteger(execution.status) && execution.status !== 0,
          label + ': corrupted input did not return a nonzero guest error');
        const errorLine = stdout.split('\n').find(line => line.startsWith('WAD LOOKUP ERROR '));
        require(errorLine && errorLine.includes(expectedError) && !stdout.includes('WAD LOOKUP OK'),
          label + ': missing expected diagnostic ' + JSON.stringify(expectedError) + '; see ' + stdoutPath);
        check.diagnostic = errorLine;
      } else {
        require(execution.status === 0 && stdout.endsWith(EXPECTED)
          && !stdout.includes('WAD LOOKUP ERROR'), 'valid metadata fixture failed; see ' + stdoutPath);
      }
      require(digest(selectedDisk) === imageHash, label + ': guest modified its disk');
      require(digest(source) === sourceHash && digest(asset.prepared_path) === asset.sha256
        && digest(binary) === binaryHash, label + ': source WAD, cached image or binary changed');
      check.success = true;
    }

    function mutate(label, changes, expectedError) {
      const image = Buffer.from(original);
      const mutations = changes.map(([word, value]) => {
        require(Number.isSafeInteger(word) && word >= 0 && word < asset.total_words,
          label + ': mutation outside meaningful image');
        const before = image.readUInt16LE(word * 2);
        require(value >= 0 && value <= 65535 && Number.isInteger(value), 'invalid mutation word');
        image.writeUInt16LE(value, word * 2);
        return { word, before, after: value };
      });
      require(!image.equals(original), label + ': mutation did not change the image');
      runCase(label, image, expectedError, mutations);
    }
    const u32 = (word, value) => [[word, value & 65535], [word + 1, value >>> 16]];

    runCase('valid', original);
    console.log('PASS cooked metadata: valid directory, hash lookup and PNAMES');
    for (let i = 0; i < HEADER_WORDS; ++i) {
      const at = asset.header_word + i;
      mutate('header-' + String(i).padStart(2, '0'),
        [[at, (original.readUInt16LE(at * 2) + 1) & 65535]],
        'cooked asset version or source mismatch');
    }
    console.log('PASS cooked metadata: all 32 header words rejected when changed');

    const directory = asset.directory_word;
    const directoryError = 'Invalid cooked directory link at lump 0';
    mutate('directory-position', u32(directory + 10, asset.source_octets + 1),
      'Cooked lump 0 lies outside the WAD');
    mutate('directory-size', u32(directory + 12, 0xffffffff),
      'Cooked lump 0 lies outside the WAD');
    mutate('directory-name-high-cell', [[directory, 256]], 'Invalid cooked name at lump 0');
    for (const [label, offset] of [['wad-address', 8], ['wad-bank', 9], ['cache-address', 14], ['cache-bank', 15]])
      mutate('directory-' + label, [[directory + offset, 1]], directoryError);
    mutate('directory-next-outside', [[directory + 16, asset.lump_count]], directoryError);
    mutate('directory-next-high', [[directory + 17, 1]], directoryError);
    mutate('directory-next-self', [[directory + 16, 0]], directoryError);
    mutate('directory-next-forward', [[directory + 16, 1]], directoryError);
    mutate('hash-head-outside', [[asset.hash_heads_word, asset.lump_count]], 'Invalid cooked hash head');
    mutate('hash-head-negative', [[asset.hash_heads_word, 65534]], 'Invalid cooked hash head');
    mutate('pnames-outside', [[asset.pnames_word, asset.lump_count]], 'Invalid cooked PNAMES patch ID');
    mutate('pnames-negative', [[asset.pnames_word, 65534]], 'Invalid cooked PNAMES patch ID');
    require(digest(disk) === asset.sha256, 'valid reference image changed');
    result.success = true;
    console.log('PASS cooked metadata: ' + (result.checks.length - 1)
      + ' corruptions rejected by explicit guest errors; source WAD and cache unchanged');
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
      timeout: { type: 'string', default: '30' },
      'max-instructions': { type: 'string', default: '100000000' },
      'skip-build': { type: 'boolean', default: false }, help: { type: 'boolean' },
    } });
    if (values.help) {
      console.log('Usage: node tools/metadata-validate.mjs [--wad FILE] [--output DIRECTORY]\n'
        + '  [--timeout SECONDS] [--max-instructions N] [--skip-build]\n'
        + 'Builds one cooked wad_lookup fixture, then checks valid and corrupted copied images in bounded Rust VM runs.\n'
        + 'DOOM_BUILD selects an isolated build directory. Source WAD and cached assets remain unchanged.');
    } else validateMetadata({ wad: values.wad, output: values.output,
      timeout: Number(values.timeout), maxInstructions: Number(values['max-instructions']),
      skipBuild: values['skip-build'] });
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}
