#!/usr/bin/env node
// Bounded native C / Rust VM tests. No Brainfuck execution and no network access.
import fs from 'node:fs';
import path from 'node:path';
import { parseArgs } from 'node:util';
import { PORT, ROOT, BUILD, WAD, WAD_MODES, RVM, RLIB, BANK, MEMORY, commands, digest, json, mkdir,
  require, writeJSON, run, build, prepareDisk, compileRust, frameRows, nativeInput } from './test-support.mjs';
import { inspectAssets, selfTestAssets, checkAssetLog } from './assets-oracle.mjs';
import { benchmark } from './benchmark.mjs';

const EXPECTED = {
  alloc_fast: 'ALLOC FAST OK zero sizes, overflow, reused cells, wide products and bank crossings\n',
  numeric: 'NUMERIC OK 6e970016 d440d0e1 1c93684a\n',
  libc: 'LIBC validation_ok 1\n',
  wad_lookup: 'WAD LOOKUP OK 1264 lumps, 350 names, 598 references, final block\n',
  wad_directory: 'WAD DIRECTORY OK 1/17 entries, batch tail, full names, EOF marker, duplicate lookup\n',
  wad_read: 'WAD READ OK odd octets, storage/RAM boundaries, bounds, decoder masks\n',
  wad_read_small: 'WAD READ SMALL OK odd octets, storage boundaries, bounds, decoder masks\n',
  wad_read_perf: 'WAD READ PERF begin\nWAD READ PERF copied\nWAD READ PERF OK 4096 texture octets, far heap, checksum 368674\n',
  asset_words: 'ASSET WORDS OK storage modes, destinations, bank boundaries, full words, bounds\n',
  graphics_cache: 'GRAPHICS OK decoded widths 1/9, bank-crossing offsets, cache purge/reload, 12 malformed cases\n',
  demo_playback: 'DEMO OK signed octets, bounded records, DEMO1 5026 9ca0bb8e\n',
  stat_storage: 'STAT STORAGE OK 32 retained snapshots, lazy allocation, wide times\n',
  info_storage: 'INFO OK 967 states, 137 objects, canonical fields and callbacks\n',
  projection_widths: 'PROJECTION OK wide wall tiers, empty spans and plane clipping\n',
  sky_widths: 'SKY OK: fixed-point midpoint and exposed sky row\n',
  event_widths: 'EVENT WIDTHS OK packed automap messages, queue copy, keyboard discrimination\n',
  texture_columns: 'TEXTURE OK: short/tall composites, last-column guard, holes, masked posts\n',
  texture_metadata: 'TEXTURE METADATA INITIALIZED\nTEXTURE METADATA OK 125 textures, 598 references, 10776 columns\n',
  keyboard_controls: 'INPUT OK: arrows/WASD, Control fire, Space use, Shift/Alt, aliases and releases\n',
  config_bindings: 'CONFIG BINDINGS OK 24 numbered names, 97 fixed names, targets and player defaults\n',
};
const { values, positionals } = parseArgs({ allowPositionals: true, options: {
  wad: { type: 'string', default: WAD }, 'wad-mode': { type: 'string', default: process.env.DOOM_WAD_MODE || 'cooked' }, output: { type: 'string' },
  timeout: { type: 'string', default: '120' }, 'max-instructions': { type: 'string', default: '2000000000' },
  program: { type: 'string', multiple: true }, 'skip-build': { type: 'boolean', default: false },
  'low-detail': { type: 'boolean', default: false }, help: { type: 'boolean' },
} });
const stage = positionals[0] || 'smoke';
const stages = ['smoke', 'fixtures', 'kernels', 'gameplay', 'assets', 'all'];
if (values.help) {
  console.log('Usage: node tools/test.mjs [' + stages.join('|') + ']\n'
    + '  [--wad FILE] [--wad-mode packed|expanded|cooked] [--output DIRECTORY] [--timeout SECONDS] [--max-instructions N]\n'
    + '  [--program NAME ...] [--skip-build] [--low-detail]\n'
    + 'The default smoke stage compares title, menu navigation and E1M1 pixels with native C.');
  process.exit(0);
}
require(positionals.length <= 1 && stages.includes(stage), 'unknown test stage: ' + stage);
const wadMode = values['wad-mode'];
require(WAD_MODES.includes(wadMode), 'unknown WAD storage mode: ' + wadMode);
require(!values['skip-build'] || !['smoke', 'all'].includes(stage),
  'smoke/all must rebuild the engine for both startup modes; omit --skip-build');
const timeout = Number(values.timeout), maxInstructions = Number(values['max-instructions']);
require(Number.isSafeInteger(timeout) && timeout > 0 && Number.isSafeInteger(maxInstructions) && maxInstructions > 0,
  'test limits must be positive integers');
const out = path.resolve(values.output || path.join(BUILD, 'tests', stage)); mkdir(out);
const wad = path.resolve(values.wad), sourceHash = digest(wad);
const disk = path.join(out, 'fixture-disk.img');
let diskMetadata;
const result = { format_version: 2, stage, success: false, checks: [], commands,
  wad: { path: wad, sha256: sourceHash }, wad_mode: wadMode,
  limits: { instructions: maxInstructions, seconds: timeout }, bank_size: BANK, memory_cells: MEMORY };
const binary = program => path.join(BUILD, 'ripple', program, program + '.bin');
const native = program => path.join(BUILD, 'native', program, program);
function buildProgram(program, target = 'both', startMap = false) {
  if (!values['skip-build']) build(program, out, { wad, wadMode, target, startMap, lowDetail: values['low-detail'] });
  if (target !== 'native') {
    diskMetadata = prepareDisk(wad, disk, { wadMode, manifest: path.join(BUILD, 'manifest.json') });
    result.disk_image = diskMetadata;
  }
}
function guest(label, program, { expected, frame = 1, script } = {}) {
  const reportPath = path.join(out, label + '.report.json'); fs.rmSync(reportPath, { force: true });
  const command = [RVM, binary(program), '--bank-size', BANK, '--memory', MEMORY,
    '--headless', '--clock', 'unthrottled', '--seed', 1, '--disk', disk, '--exit-code',
    '--max-instructions', maxInstructions, '--timeout', timeout, '--report', reportPath];
  const raw = path.join(out, label + '.rgb565');
  if (expected) {
    const png = path.join(out, label + '.png');
    fs.rmSync(raw, { force: true }); fs.rmSync(png, { force: true });
    command.push('--screenshot', png, '--raw-frame', raw, '--capture-frame', frame, '--expect-frame', expected);
  }
  if (script) command.push('--input-script', script);
  const output = run(label, command, out, timeout + 10), report = json(reportPath);
  require(report.success === true && report.stop_reason === (expected ? 'captured_frame' : 'halted'), label + ': VM failed');
  for (const [key, value] of Object.entries({ bank_size: BANK, memory_cells: MEMORY, seed: 1,
    disk_sha256: diskMetadata.sha256, tick_hz: 35, clock: 'unthrottled',
    binary_sha256: digest(binary(program)), input_overflow: false }))
    require(report[key] === value, label + ': unexpected report ' + key);
  if (expected) {
    require(report.width === 320 && report.height === 200 && report.frame === frame, label + ': wrong frame dimensions/number');
    require(fs.readFileSync(raw).equals(fs.readFileSync(expected)), label + ': RGB565 capture differs');
  }
  if (script) require(report.input_script_sha256 === digest(script), label + ': replay identity differs');
  require(digest(disk) === diskMetadata.sha256, label + ': guest changed WAD disk');
  result.checks.push({ label, vm_report: report });
  return { output, report };
}
function engineCase(name, mode, frame, state, script) {
  const label = 'engine-' + name, raw = path.join(out, label + '-native.rgb565');
  fs.rmSync(raw, { force: true });
  const command = [native('engine'), raw, frame, mode];
  if (script) {
    const input = path.join(out, label + '.input.txt'); nativeInput(script, input); command.push(input);
  }
  const nativeLog = run(label + '-native', command, out, timeout);
  require(fs.statSync(raw).size === 128000, label + ': missing or incorrect native frame');
  const { output, report } = guest(label + '-ripple', 'engine', { expected: raw, frame, script });
  const reference = frameRows(nativeLog), actual = frameRows(output), final = reference.at(-1);
  require(reference.length > 0 && JSON.stringify(reference) === JSON.stringify(actual), label + ': frame/state logs differ');
  require(report.tick === final[1] && final[0] === frame && final[2] === state, label + ': wrong frame/tick/state');
  if (state === 0) require(final[3] === 1 && final[4] === 1, label + ': did not reach E1M1');
  if (name === 'menu-start') require(reference.every(([n, tick, game]) => tick === n + 1 && game === (n < 8 ? 3 : 0)),
    'menu replay did not enter world at tick 9');
  for (const log of [nativeLog, output]) require(log.includes('DOOM PORT WAD ' + sourceHash)
    && !log.includes('DOOM ERROR:'), label + ': engine error or incorrect WAD');
  result.checks.push({ label, frames: reference, raw_sha256: digest(raw), native_binary_sha256: digest(native('engine')) });
  console.log('PASS ' + label + ': exact native/Rust VM pixels and logical state');
}
function smoke() {
  buildProgram('engine');
  engineCase('title', 'title', 1, 3);
  engineCase('menu-start', 'title', 10, 0, path.join(PORT, 'tests/menu-start.json'));
  buildProgram('engine', 'both', true);
  engineCase('map', 'map', 1, 0);
}
function assets() {
  buildProgram('assets');
  const oracle = inspectAssets(fs.readFileSync(wad)), checks = selfTestAssets(fs.readFileSync(wad));
  const expected = path.join(out, 'assets-oracle.rgb565'); fs.writeFileSync(expected, oracle.raw);
  const reference = path.join(out, 'assets-native.rgb565'); fs.rmSync(reference, { force: true });
  const log = run('assets-native', [native('assets'), wad, reference], out, timeout);
  require(fs.readFileSync(reference).equals(oracle.raw), 'native assets differ from independent WAD oracle');
  checkAssetLog(log, oracle.metrics);
  const actual = guest('assets-ripple', 'assets', { expected }); checkAssetLog(actual.output, oracle.metrics);
  const { raw, ...metadata } = oracle;
  result.checks.push({ label: 'assets-independent-oracle', ...metadata, oracle_self_tests: checks });
  writeJSON(path.join(out, 'assets-oracle.json'), { ...metadata, oracle_self_tests: checks });
  console.log('PASS assets: malformed data, geometry/texture references and exact TITLEPIC pixels');
}
function fixtures() {
  const selection = values.program || [...Object.keys(EXPECTED), 'map_decode', 'world_widths', 'clock_widths', 'assets'];
  for (const program of selection) {
    if (program === 'assets') { assets(); continue; }
    if (['map_decode', 'world_widths', 'clock_widths'].includes(program)) {
      buildProgram(program, 'native');
      const output = run(program + '-native', [native(program), ...(program === 'map_decode' ? [wad] : [])], out, timeout);
      result.checks.push({ label: program + '-native', output }); console.log('PASS ' + output.trim());
      continue;
    }
    require(Object.hasOwn(EXPECTED, program), 'unknown C fixture: ' + program);
    buildProgram(program);
    require(run(program + '-native', [native(program)], out, timeout) === EXPECTED[program], program + ': native diagnostic differs');
    require(guest(program + '-ripple', program).output === EXPECTED[program], program + ': guest diagnostic differs');
    result.checks.push({ label: program + '-exact-output', expected: EXPECTED[program] });
    console.log('PASS ' + program + ': native C and Rust VM');
  }
}
function kernels() {
  for (const kind of ['render', 'memory', 'raster', 'fixed']) {
    const program = kind + '_kernels';
    if (kind !== 'fixed') {
      buildProgram(program);
      run(program + '-native', [native(program)], out, timeout);
    }
    const target = kind === 'render' || kind === 'memory' ? path.dirname(binary(program)) : path.join(out, kind);
    mkdir(target);
    if (kind === 'raster') guest('raster-kernels-ripple', program);
    if (kind === 'fixed' || kind === 'raster') {
      const source = path.join(PORT, 'src', kind + '_fast.asm'), asm = path.join(target, kind + '_test.asm');
      fs.copyFileSync(source, asm);
      const object = path.join(target, kind + '_test.pobj'), image = path.join(target, kind + '_test.bin');
      run(kind + '-assemble', [path.join(ROOT, 'src/ripple-asm/target/release/rasm'), 'assemble', asm, '-o', object, '-b', BANK], out, 180);
      run(kind + '-link', [path.join(ROOT, 'src/ripple-asm/target/release/rlink'), object, '-o', image, '-b', BANK], out, 180);
    }
    const source = path.join(PORT, kind === 'render' || kind === 'memory' ? 'tests/' + program + '.rs'
      : 'tools/test_' + (kind === 'fixed' ? 'fixed_kernel' : program) + '.rs');
    const driver = path.join(target, 'verify-' + kind); compileRust(source, driver, out);
    run(kind + '-kernels', [driver, target], out, 180);
    const kernelReport = json(path.join(target, kind === 'render' || kind === 'memory' ? 'kernel-results.json' : 'results.json'));
    require(kernelReport.success === true, kind + ': kernel report failed');
    result.checks.push({ label: kind + '-kernels', report: kernelReport, oracle_sha256: digest(source),
      assembly_sha256: digest(path.join(PORT, 'src', kind + '_fast.asm')), vm_library_sha256: digest(RLIB) });
    console.log('PASS ' + kind + ' kernels: independent oracle, bank boundaries, guards and C ABI');
  }
  for (const kind of ['word_read', 'metadata_validate']) {
    const label = kind.replaceAll('_', '-');
    const target = path.join(out, kind);
    mkdir(target);
    const source = path.join(PORT, 'src', kind + '_fast.asm');
    const object = path.join(target, kind + '_test.pobj');
    const image = path.join(target, kind + '_test.bin');
    run(label + '-assemble', [path.join(ROOT, 'src/ripple-asm/target/release/rasm'),
      'assemble', source, '-o', object, '-b', BANK], out, 60);
    run(label + '-link', [path.join(ROOT, 'src/ripple-asm/target/release/rlink'),
      object, '-o', image, '-b', BANK], out, 60);
    const driverSource = path.join(PORT, 'tests', kind + '_kernels.rs');
    const driver = path.join(target, 'verify-' + label);
    compileRust(driverSource, driver, out);
    run(label + '-kernels', [driver, target], out, 60);
    const kernelReport = json(path.join(target, label + '-kernel-results.json'));
    require(kernelReport.success === true, label + ' kernel report failed');
    result.checks.push({ label: label + '-kernels', report: kernelReport,
      oracle_sha256: digest(driverSource), assembly_sha256: digest(source), vm_library_sha256: digest(RLIB) });
    console.log('PASS ' + label + ' kernel: independent oracle, bank edges, pointer tags and saved registers');
  }
}
function gameplay() {
  buildProgram('engine', 'both', true);
  for (const [label, frames] of [['gameplay-long', 180], ['arrow-controls', 32], ['detail-toggle', 32]]) {
    const report = benchmark({ wad, wadMode, input: path.join(PORT, 'tests', label + '.json'), frames, output: path.join(out, label) });
    result.checks.push({ label, report });
  }
}
try {
  if (stage === 'all') { fixtures(); kernels(); smoke(); gameplay(); }
  else ({ smoke, fixtures, kernels, gameplay, assets })[stage]();
  require(digest(wad) === sourceHash && (!diskMetadata || digest(disk) === diskMetadata.sha256), 'WAD changed during tests');
  result.success = true;
  console.log('All ' + stage + ' tests passed; ' + path.join(out, 'results.json'));
} catch (error) {
  result.error = error.message; console.error(error.message); process.exitCode = 1;
} finally { writeJSON(path.join(out, 'results.json'), result); }
