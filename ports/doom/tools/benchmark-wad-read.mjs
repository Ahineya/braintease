#!/usr/bin/env node
// Compare real packed/expanded/cooked readers with unchanged 64,000-cell banks.
// Fixtures measure reader throughput/overhead, not Doom boot time.
import fs from 'node:fs';
import path from 'node:path';
import { parseArgs } from 'node:util';
import { spawn } from 'node:child_process';
import { PORT, ROOT, BUILD, WAD, WAD_MODES, RVM, BANK, MEMORY, commands,
  digest, json, mkdir, require, run, prepareDisk, writeJSON } from './test-support.mjs';

const { values } = parseArgs({ options: {
  wad: { type: 'string', default: WAD }, output: { type: 'string' },
  program: { type: 'string', default: 'wad_read_perf' },
  mode: { type: 'string', multiple: true }, timeout: { type: 'string', default: '60' },
  samples: { type: 'string', default: '3' }, help: { type: 'boolean' },
} });
if (values.help) {
  console.log('Usage: node tools/benchmark-wad-read.mjs [--mode packed|expanded|cooked ...]\n'
    + '  [--wad FILE] [--program wad_read_perf|wad_read_small] [--output DIRECTORY] [--timeout 60] [--samples 3]\n'
    + 'Builds isolated reader fixtures; compares native, Rust VM and bounded BF output.\n'
    + 'Bank size stays 64000 with 256 banks. This is a reader microbenchmark, not a Doom boot estimate.');
  process.exit(0);
}
const modes = values.mode || WAD_MODES;
const program = values.program;
require(['wad_read_perf', 'wad_read_small'].includes(program), 'invalid reader benchmark program');
const timeout = Number(values.timeout), samples = Number(values.samples);
require(modes.length > 0 && modes.every(mode => WAD_MODES.includes(mode)), 'invalid storage mode');
require(new Set(modes).size === modes.length, 'duplicate storage modes');
require(Number.isSafeInteger(timeout) && timeout > 0, 'timeout must be a positive integer');
require(Number.isSafeInteger(samples) && samples > 0, 'samples must be a positive integer');
const out = path.resolve(values.output || path.join(BUILD, 'wad-read-benchmark'));
const wad = path.resolve(values.wad), sourceHash = digest(wad);
const expected = program === 'wad_read_small'
  ? 'WAD READ SMALL OK odd octets, storage boundaries, bounds, decoder masks\n'
  : 'WAD READ PERF begin\nWAD READ PERF copied\nWAD READ PERF OK 4096 texture octets, far heap, checksum 368674\n';
const result = { format_version: 1, success: false, commands, modes: [],
  source: { path: wad, sha256: sourceHash }, bank_size: BANK, memory_cells: MEMORY,
  program, timeout_seconds: timeout, samples,
  scope: program === 'wad_read_perf'
    ? '4096 texture octets to RAM bank 136, including startup and identical checksum verification. Code remains in low banks; no full-boot extrapolation.'
    : 'Complete short reader fixture, including startup/checks. Code and stack are in low banks; no late-Doom address or full-boot extrapolation.' };
mkdir(out);
// Host output is flushed as it is produced. Timestamp complete markers so
// checksum verification and BF image parsing do not dominate read timings.
// The interval includes the fixed guest overhead of printing the end marker.
async function runBrainfuck(label, command, directory) {
  const stdout = path.join(directory, label + '.stdout.log');
  const stderr = path.join(directory, label + '.stderr.log');
  const handles = [fs.openSync(stdout, 'w'), fs.openSync(stderr, 'w')];
  const record = { label, command: command.map(String), stdout, stderr, started_at: new Date().toISOString() };
  commands.push(record);
  const start = performance.now();
  const markers = {};
  let output = '';
  try {
    await new Promise((resolve, reject) => {
      const child = spawn(record.command[0], record.command.slice(1), {
        cwd: ROOT, stdio: ['ignore', 'pipe', handles[1]],
        timeout: (timeout + 15) * 1000, killSignal: 'SIGKILL',
      });
      child.stdout.setEncoding('utf8');
      child.stdout.on('data', chunk => {
        fs.writeSync(handles[0], chunk);
        output += chunk;
        for (const marker of ['begin', 'copied'])
          if (markers[marker] === undefined && output.includes('WAD READ PERF ' + marker + '\n'))
            markers[marker] = (performance.now() - start) / 1000;
      });
      child.on('error', reject);
      child.on('close', (code, signal) => {
        record.exit_code = code;
        record.signal = signal;
        if (code === 0) resolve();
        else reject(new Error(label + ' failed (exit ' + code + ', signal ' + signal + '); see ' + stdout + ' and ' + stderr));
      });
    });
  } finally {
    handles.forEach(handle => fs.closeSync(handle));
    record.seconds = (performance.now() - start) / 1000;
  }
  return { output, read_marker_seconds: markers.copied - markers.begin };
}
try {
  for (const mode of modes) {
    const directory = path.join(out, mode), build = path.join(directory, 'build');
    mkdir(directory);
    const make = ['make', '-C', PORT, 'PROGRAM=' + program, 'WAD=' + wad,
      'WAD_MODE=' + mode, 'BUILD=' + build];
    run('build', [...make, 'TARGET=both', 'all'], directory, 900);
    run('build-bf', [...make, 'TARGET=bf', 'all'], directory, 900);
    const disk = path.join(directory, 'disk.img');
    const metadata = prepareDisk(wad, disk, { wadMode: mode, manifest: path.join(build, 'manifest.json') });
    const executable = path.join(build, 'native', program, program);
    const binary = path.join(build, 'ripple', program, program + '.bin');
    const brainfuck = path.join(build, 'ripple', program, program + '.bf');
    require(run('native', [executable], directory, timeout) === expected, mode + ': native output differs');
    const vmReport = path.join(directory, 'rvm.json');
    require(run('rvm', [RVM, binary, '--bank-size', BANK, '--memory', MEMORY, '--headless',
      '--disk', disk, '--timeout', timeout, '--max-instructions', 20000000, '--exit-code',
      '--report', vmReport], directory, timeout + 10) === expected, mode + ': RVM output differs');
    const vm = json(vmReport);
    require(vm.success && vm.stop_reason === 'halted', mode + ': RVM did not halt');
    const tape = run('tape-size', ['node', path.join(PORT, 'tools/build-support.mjs'),
      'bf-tape-cells', brainfuck], directory, 10).trim();
    const record = { mode, disk_image: metadata, rvm: vm, bf: [] };
    result.modes.push(record);
    for (let sample = 1; sample <= samples; ++sample) {
      const report = path.join(directory, 'bf-' + sample + '.json');
      const timed = await runBrainfuck('bf-' + sample, [path.join(ROOT, 'target/release/rbf'), brainfuck,
        '--headless', '--disk', disk, '--max-tape-cells', tape, '--timeout', timeout,
        '--max-instructions', 20000000, '--exit-code', '--report', report], directory);
      require(timed.output === expected, mode + ': BF output differs');
      const measured = json(report);
      require(measured.success && measured.stop_reason === 'halted', mode + ': BF did not halt');
      require(measured.ripple_instructions === vm.instructions, mode + ': BF/RVM instruction counts differ');
      require(measured.layout.bank_size === BANK && measured.layout.memory_banks === 256,
        mode + ': BF memory layout differs');
      require(measured.disk_sha256 === metadata.sha256 && digest(disk) === metadata.sha256,
        mode + ': disk identity changed');
      if (program === 'wad_read_perf') {
        require(Number.isFinite(timed.read_marker_seconds) && timed.read_marker_seconds >= 0,
          mode + ': missing read markers');
        measured.read_marker_seconds = timed.read_marker_seconds;
      }
      record.bf.push(measured);
    }
    const times = record.bf.map(sample => sample.elapsed_seconds).sort((a, b) => a - b);
    record.median_seconds = (times[Math.floor(times.length / 2)] + times[Math.floor((times.length - 1) / 2)]) / 2;
    if (program === 'wad_read_perf') {
      const reads = record.bf.map(sample => sample.read_marker_seconds).sort((a, b) => a - b);
      record.median_read_marker_seconds = (reads[Math.floor(reads.length / 2)] + reads[Math.floor((reads.length - 1) / 2)]) / 2;
    }
    console.log(mode + ': ' + record.median_seconds.toFixed(3) + 's median; '
      + (record.median_read_marker_seconds !== undefined ? record.median_read_marker_seconds.toFixed(3) + 's read markers; ' : '')
      + vm.instructions.toLocaleString() + ' guest instructions; exact native/RVM/BF output');
  }
  require(digest(wad) === sourceHash, 'source WAD changed');
  result.success = true;
} catch (error) {
  result.error = error.message;
  console.error(error.message);
  process.exitCode = 1;
} finally { writeJSON(path.join(out, 'results.json'), result); }
