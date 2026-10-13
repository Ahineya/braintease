#!/usr/bin/env node
// Compare every replay frame with native C, then report production-runner timings.
// Optional PC profiling runs separately so instrumentation does not affect timings.
import fs from 'node:fs';
import path from 'node:path';
import { parseArgs } from 'node:util';
import { pathToFileURL } from 'node:url';
import { PORT, BUILD, WAD, WAD_MODES, RLIB, BANK, MEMORY, digest, json, mkdir, require,
  writeJSON, run, prepareDisk, compileRust, frameRows, nativeInput } from './test-support.mjs';

const OPCODES = ['HALT', 'ADD', 'SUB', 'AND', 'OR', 'XOR', 'SLL', 'SRL', 'SLT', 'SLTU',
  'ADDI', 'ANDI', 'ORI', 'XORI', 'LI', 'SLLI', 'SRLI', 'LOAD', 'STORE', 'JAL', 'JALR',
  'BEQ', 'BNE', 'BLT', 'BGE', 'BRK', 'MUL', 'DIV', 'MOD', 'MULI', 'DIVI', 'MODI', 'LOADC', 'STORC'];
OPCODES[66] = 'NOP';
const count = (map, key, amount) => map[key] = (map[key] || 0) + amount;
const sorted = map => Object.fromEntries(Object.entries(map).sort((a, b) => b[1] - a[1]));
const median = rows => {
  rows = [...rows].sort((a, b) => a - b);
  return (rows[Math.floor(rows.length / 2)] + rows[Math.floor((rows.length - 1) / 2)]) / 2;
};

// The oracle always renders the original 320x200 image. Sample that completed
// image independently; the reduced guest must skip these pixels while drawing.
export function sampleReference(raw, scale) {
  require([1, 2, 4].includes(scale), 'reference sample scale must be 1, 2 or 4');
  require(raw.length === 320 * 200 * 2, 'native reference must be a full 320x200 RGB565 image');
  if (scale === 1) return raw;
  const result = Buffer.alloc(320 / scale * (200 / scale) * 2);
  for (let y = 0; y < 200 / scale; y++) for (let x = 0; x < 320 / scale; x++)
    result.writeUInt16LE(raw.readUInt16LE(((y * scale) * 320 + x * scale) * 2), (y * (320 / scale) + x) * 2);
  return result;
}

// Object labels recover static function boundaries omitted from binary DEBUG.
// Reject stale objects rather than attributing samples to invented boundaries.
export function analyzeProfile(binary, hitPath, objectDir = null, archives = []) {
  const raw = fs.readFileSync(binary);
  require(raw.subarray(0, 5).toString() === 'RLINK', 'expected RLINK image');
  const bank = raw.readUInt16LE(5), entry = raw.readUInt32LE(7), length = raw.readUInt32LE(11);
  let offset = 15 + length * 8;
  offset += 4 + raw.readUInt32LE(offset);
  require(raw.subarray(offset, offset + 5).toString() === 'DEBUG', 'missing DEBUG symbols');
  offset += 5;
  const symbolCount = raw.readUInt32LE(offset), symbols = new Map();
  offset += 4;
  for (let i = 0; i < symbolCount; i++) {
    const size = raw.readUInt32LE(offset); offset += 4;
    const name = raw.subarray(offset, offset + size).toString(); offset += size;
    symbols.set(name, raw.readUInt32LE(offset)); offset += 4;
  }
  const hitData = fs.readFileSync(hitPath);
  require(hitData.length === length * 8, 'PC sample count differs from instruction image');
  const hits = Array.from({ length }, (_, i) => Number(hitData.readBigUInt64LE(i * 8)));
  require(hits.every(Number.isSafeInteger), 'PC count exceeds exact JavaScript integer range');
  const functions = [];
  function objectRanges(object, origin) {
    const labels = Object.entries(object.labels).map(([name, info]) => [name, Math.floor(info.absolute_address / 4)]);
    const size = object.instructions.length;
    const names = new Map();
    for (const [name, pc] of labels) if (!name.startsWith('L_') && !name.startsWith('.') && pc < size) {
      if (!names.has(pc)) names.set(pc, []);
      names.get(pc).push(name);
    }
    const starts = [...names.keys()].sort((a, b) => a - b);
    if (!starts.length) return;
    // RCC reachability can discard functions, and startup placement moves
    // initializers/functions independently even when the object fits one bank.
    // Each metadata range therefore needs its own binary-symbol anchor.
    let boundaries = starts;
    if (object.rcc) {
      boundaries = object.rcc.code.map(name => {
        require(object.labels[name], origin + ': missing RCC code label ' + name);
        return Math.floor(object.labels[name].absolute_address / 4);
      }).sort((a, b) => a - b);
      require(boundaries[0] === 0 && boundaries.every((pc, i) => pc < size && (!i || pc > boundaries[i - 1])),
        origin + ': invalid RCC function boundaries');
    }
    const segments = !object.rcc && size <= bank ? [[0, size]]
      : boundaries.map((start, i) => [start, boundaries[i + 1] ?? size]);
    for (const [begin, end] of segments) {
      const anchors = new Set(labels.filter(([name, pc]) => pc >= begin && pc < end && symbols.has(name))
        .map(([name, pc]) => symbols.get(name) - pc));
      if (!anchors.size) continue;
      require(anchors.size === 1, origin + ': inconsistent object/binary anchors; save matching objects');
      const shift = [...anchors][0];
      for (let pc = begin; pc < end; pc++) {
        const inst = object.instructions[pc], at = 15 + (pc + shift) * 8;
        const reference = object.unresolved_references?.[pc];
        const moved = new Set();
        if (inst.opcode === 19 || reference?.ref_type === 'absolute') {
          moved.add(2); moved.add(3);
        } else if (['code_bank', 'code_offset'].includes(reference?.ref_type)
                   || (reference?.ref_type === 'data' && inst.opcode === 14)) moved.add(2);
        else if (reference) moved.add(3);
        require(pc + shift >= 0 && pc + shift < length
          && raw[at] === inst.opcode && raw[at + 1] === (inst.word0 ?? 0)
          && [1, 2, 3].every(word => moved.has(word) || raw.readUInt16LE(at + word * 2) === inst['word' + word]),
        origin + ': stale object at local PC ' + pc);
      }
      starts.forEach((start, i) => {
        if (start < begin || start >= end) return;
        const symbol = names.get(start).sort()[0];
        functions.push({ name: symbol.replace(/^__rcc_local_.*?_[0-9a-f]{32}_/, ''), symbol,
          object: origin, start: start + shift, end: (starts[i + 1] ?? size) + shift,
          instructions: 0, spill_load_store: 0, spill_with_address_setup: 0, opcodes: {} });
      });
    }
  }
  if (objectDir) for (const file of fs.readdirSync(objectDir).filter(file => file.endsWith('.pobj')).sort())
    objectRanges(json(path.join(objectDir, file)), file);
  for (const archive of archives) for (const row of json(archive).objects)
    objectRanges(row.object, path.basename(archive) + ':' + row.name);
  functions.sort((a, b) => a.start - b.start);
  functions.forEach((fn, i) => require(!i || functions[i - 1].end <= fn.start, 'overlapping object function ranges'));
  const instruction = pc => {
    const at = 15 + pc * 8;
    return [raw[at], raw.readUInt16LE(at + 2), raw.readUInt16LE(at + 4), raw.readUInt16LE(at + 6)];
  };
  let index = 0, total = 0, attributed = 0;
  const opcodes = {}, hot = [];
  hits.forEach((times, pc) => {
    if (!times) return;
    const [op, a, b, c] = instruction(pc), name = OPCODES[op] || String(op);
    total += times; count(opcodes, name, times);
    while (index < functions.length && functions[index].end <= pc) index++;
    const fn = functions[index]?.start <= pc ? functions[index] : undefined;
    if (fn) {
      attributed += times; fn.instructions += times; count(fn.opcodes, name, times);
      if ((op === 17 || op === 18) && b === 28 && c === 27) {
        fn.spill_load_store += times;
        let cost = times;
        if (pc >= 1) {
          const previous = instruction(pc - 1);
          if (previous.slice(0, 3).join(',') === '10,27,30')
            cost += hits[pc - 1];
          else if (pc >= 2 && instruction(pc - 2).join(',') === '1,27,30,0'
                   && previous.slice(0, 3).join(',') === '10,27,27')
            cost += hits[pc - 2] + hits[pc - 1];
        }
        fn.spill_with_address_setup += cost;
      }
    }
    hot.push({ pc, hits: times, instruction: [name, a, b, c], function: fn?.name ?? null });
  });
  require(Number.isSafeInteger(total), 'sample total exceeds exact JavaScript integer range');
  const rows = functions.filter(fn => fn.instructions).sort((a, b) => b.instructions - a.instructions)
    .map(fn => ({ ...fn, percent: fn.instructions * 100 / total, opcodes: sorted(fn.opcodes) }));
  hot.sort((a, b) => b.hits - a.hits);
  return { binary_sha256: digest(binary), instruction_count: length, bank_size: bank, entry,
    total, attributed, unattributed: total - attributed, functions: rows,
    spill_load_store: rows.reduce((sum, fn) => sum + fn.spill_load_store, 0),
    spill_with_address_setup: rows.reduce((sum, fn) => sum + fn.spill_with_address_setup, 0),
    opcodes: sorted(opcodes), hot_instructions: hot.slice(0, 100),
    hot_bitwise_instructions: hot.filter(row => ['AND', 'ANDI', 'OR', 'ORI', 'XOR', 'XORI'].includes(row.instruction[0])).slice(0, 100),
    hot_shift_instructions: hot.filter(row => ['SLL', 'SLLI', 'SRL', 'SRLI'].includes(row.instruction[0])).slice(0, 100) };
}

export function benchmark(options = {}) {
  const settings = { binary: path.join(BUILD, 'ripple/engine/engine.bin'),
    native: path.join(BUILD, 'native/engine/engine'), objects: path.join(BUILD, 'ripple/engine'),
    wad: WAD, input: path.join(PORT, 'tests/gameplay.json'), output: path.join(BUILD, 'gameplay-benchmark'),
    frames: 32, mode: 'map', sampleScale: 1, wadMode: process.env.DOOM_WAD_MODE || 'cooked', manifest: path.join(BUILD, 'manifest.json'), profiles: [], archives: [], ...options };
  require(Number.isSafeInteger(settings.frames) && settings.frames >= 2, 'benchmark needs at least two frames');
  require(settings.profiles.every(frame => Number.isSafeInteger(frame) && frame >= 1 && frame <= settings.frames),
    'profile frames must be inside benchmark range');
  require(['map', 'title', 'demo1'].includes(settings.mode), 'invalid native start mode');
  require(WAD_MODES.includes(settings.wadMode), 'invalid WAD storage mode');
  require([1, 2, 4].includes(settings.sampleScale), 'reference sample scale must be 1, 2 or 4');
  const out = path.resolve(settings.output); mkdir(out);
  const result = { success: false };
  try {
    const script = path.join(out, 'input.json'), nativeScript = path.join(out, 'input.txt');
    if (path.resolve(settings.input) !== script) fs.copyFileSync(settings.input, script);
    nativeInput(script, nativeScript);
    const binary = path.join(out, 'engine.bin'), native = path.join(out, 'engine-native');
    if (path.resolve(settings.binary) !== binary) fs.copyFileSync(settings.binary, binary);
    if (path.resolve(settings.native) !== native) fs.copyFileSync(settings.native, native);
    fs.chmodSync(native, fs.statSync(settings.native).mode);
    const disk = path.join(out, 'disk.img'), diskMetadata = prepareDisk(settings.wad, disk,
      { wadMode: settings.wadMode, manifest: settings.manifest });
    const wadHash = digest(settings.wad), driver = path.join(out, 'profile_vm');
    compileRust(path.join(PORT, 'tools/profile_vm.rs'), driver, out);
    console.log('Measuring ' + settings.frames + ' frames with the production Rust VM runner');
    const guestLog = run('timing', [driver, binary, disk, script, path.join(out, 'timing'), settings.frames], out, 900);
    const rows = fs.readFileSync(path.join(out, 'timing/frames.jsonl'), 'utf8').trim().split('\n').map(JSON.parse);
    const guestFrames = frameRows(guestLog);
    require(rows.length === settings.frames && guestFrames.length === settings.frames, 'missing guest frame records');
    require(rows.every(row => row.width === 320 / settings.sampleScale && row.height === 200 / settings.sampleScale),
      'guest presentation dimensions do not match the selected resolution');
    Object.assign(result, { frames: rows, checks: [], binary_sha256: digest(binary), native_sha256: digest(native),
      script_sha256: digest(script), vm_library_sha256: digest(RLIB), disk_image: diskMetadata,
      wad_mode: settings.wadMode, bank_size: BANK, memory_cells: MEMORY, seed: 1,
      resolution_scale: settings.sampleScale, width: 320 / settings.sampleScale, height: 200 / settings.sampleScale });
    for (let frame = 1; frame <= settings.frames; frame++) {
      const raw = path.join(out, 'native-' + frame + '.rgb565');
      fs.rmSync(raw, { force: true });
      const log = run('native-' + frame, [native, raw, frame, settings.mode, nativeScript], out, 180);
      const referenceFrames = frameRows(log), actual = path.join(out, 'timing/frame-' + frame + '.rgb565');
      const expectedBytes = sampleReference(fs.readFileSync(raw), settings.sampleScale), actualBytes = fs.readFileSync(actual);
      const sampled = settings.sampleScale === 1 ? raw : path.join(out, 'sampled-native-' + frame + '.rgb565');
      if (sampled !== raw) fs.writeFileSync(sampled, expectedBytes);
      const stateMatches = JSON.stringify(referenceFrames) === JSON.stringify(guestFrames.slice(0, frame));
      if (!expectedBytes.equals(actualBytes) || !stateMatches) {
        let differing = 0;
        for (let offset = 0; offset < Math.max(expectedBytes.length, actualBytes.length); offset += 2)
          if (!expectedBytes.subarray(offset, offset + 2).equals(actualBytes.subarray(offset, offset + 2))) differing++;
        Object.assign(result, { failed_frame: frame, differing_pixels: differing,
          expected_raw: sampled, actual_raw: actual, state_matches: stateMatches });
        throw new Error('frame ' + frame + ': native pixels or state records differ');
      }
      result.checks.push({ frame, raw_sha256: digest(actual), full_reference_sha256: digest(raw), state: referenceFrames.at(-1) });
    }
    result.profiles = [];
    if (settings.profiles.length) {
      const profile = path.join(out, 'profile');
      run('profile', [driver, binary, disk, script, profile, Math.max(...settings.profiles), ...settings.profiles], out, 900);
      for (const frame of settings.profiles) {
        require(fs.readFileSync(path.join(profile, 'frame-' + frame + '.rgb565'))
          .equals(fs.readFileSync(path.join(out, 'timing/frame-' + frame + '.rgb565'))), 'profile replay diverged');
        const report = path.join(out, 'profile-' + frame + '.json');
        writeJSON(report, analyzeProfile(binary, path.join(profile, 'frame-' + frame + '.hits.u64'),
          settings.objects, settings.archives));
        result.profiles.push(report);
      }
    }
    require(digest(disk) === diskMetadata.sha256 && digest(settings.wad) === wadHash, 'WAD changed during benchmark');
    const seconds = median(rows.slice(1).map(row => row.seconds));
    Object.assign(result, { success: true, steady_median_seconds: seconds, steady_fps: 1 / seconds,
      steady_median_instructions: median(rows.slice(1).map(row => row.instructions)),
      measurement: 'Production runner; unthrottled logical clock; every presented frame compared with full native C' +
        (settings.sampleScale === 1 ? '' : ' top-left samples at scale ' + settings.sampleScale) + '; startup excluded.' });
    console.log('PASS ' + settings.frames + ' exact native' + (settings.sampleScale === 1 ? '' : ' sampled') + ' frames; median ' + seconds.toFixed(4)
      + 's (' + (1 / seconds).toFixed(2) + ' fps), ' + result.steady_median_instructions.toLocaleString() + ' instructions');
    return result;
  } catch (error) {
    result.error = error.message;
    throw error;
  } finally { writeJSON(path.join(out, 'results.json'), result); }
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  const { values } = parseArgs({ options: Object.fromEntries(
    ['binary', 'native', 'objects', 'wad', 'wad-mode', 'manifest', 'input', 'output', 'frames', 'mode', 'sample-scale', 'profile-frame', 'archive', 'help']
      .map(name => [name, { type: name === 'help' ? 'boolean' : 'string', multiple: ['profile-frame', 'archive'].includes(name) }])) });
  if (values.help) console.log('Usage: node tools/benchmark.mjs [--frames 32] [--input tests/gameplay.json]\n'
    + '  [--binary FILE] [--native FILE] [--wad FILE] [--wad-mode packed|expanded|cooked] [--manifest FILE] [--mode map|title|demo1]\n'
    + '  [--output DIRECTORY] [--sample-scale 1|2|4] [--profile-frame N ...] [--objects DIRECTORY] [--archive FILE ...]\n'
    + 'The native reference must use full resolution; --sample-scale selects top-left samples for a reduced guest.');
  else try {
    benchmark({ ...values, sampleScale: Number(values['sample-scale'] ?? 1), wadMode: values['wad-mode'] ?? (process.env.DOOM_WAD_MODE || 'cooked'), frames: Number(values.frames ?? 32), profiles: (values['profile-frame'] || []).map(Number),
      archives: values.archive || [] });
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
