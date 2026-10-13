#!/usr/bin/env node
// Compare packed values and both guest paths against frozen original C loops.
// All runs are bounded Rust VM/native runs. Brainfuck is never executed here.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import { parseArgs } from 'node:util';
import { fileURLToPath } from 'node:url';
import { packRendererTables } from './renderer-tables.mjs';
import { validateRendererTables } from './renderer-tables-validate.mjs';
import { PORT, BUILD, WAD, BANK, MEMORY, RVM, commands, run, mkdir,
  prepareDisk, json, digest, writeJSON, require } from './test-support.mjs';

function comparePacked(buffer, reference) {
  const u16 = offset => buffer.readUInt16LE(offset * 2);
  const u32 = offset => buffer.readUInt32LE(offset * 2);
  const i32 = offset => buffer.readInt32LE(offset * 2);
  assert.deepEqual(Array.from({ length: 2048 }, (_, i) => u16(u32(4) + i)), reference.zlight,
    'prepared depth lights differ from the original C algorithm');
  for (let index = 0; index < 18; ++index) {
    const view = reference.views[index], record = 16 + index * 16;
    assert.deepEqual(Array.from({ length: 5 }, (_, i) => u16(record + i)),
      [view.blocks, view.detail, view.scaledwidth, view.height, view.width], 'prepared dimensions');
    const fields = ['viewangletox', 'xtoviewangle', 'yslope', 'distscale', 'scalelight'];
    fields.forEach((field, f) => {
      const offset = u32(record + 6 + f * 2), wide = f > 0 && f < 4;
      const read = f === 1 ? u32 : wide ? i32 : u16;
      assert.deepEqual(Array.from({ length: view[field].length }, (_, i) => read(offset + i * (wide ? 2 : 1))),
        view[field], `prepared ${field} differs at blocks=${view.blocks}, detail=${view.detail}`);
    });
  }
}

export function validateRendererMetadata({ wad = WAD, output, timeout = 180,
  maxInstructions = 2000000000, tiny = false } = {}) {
  require(Number.isSafeInteger(timeout) && timeout > 0, 'timeout must be a positive integer');
  require(Number.isSafeInteger(maxInstructions) && maxInstructions > 0, 'instruction limit must be positive');
  const out = path.resolve(output || path.join(BUILD, 'tests', tiny ? 'renderer-metadata-tiny' : 'renderer-metadata-validation'));
  const preparedBuild = path.join(out, 'prepared'), referenceBuild = path.join(out, 'reference');
  const fallbackBuild = path.join(out, 'fallback');
  const source = fs.realpathSync(wad), sourceHash = digest(source);
  mkdir(out);
  const result = { format_version: 1, success: false, tiny, source: { path: source, sha256: sourceHash },
    bank_size: BANK, memory_cells: MEMORY, limits: { seconds: timeout, instructions: maxInstructions },
    checks: [], commands };
  try {
    run('build-frozen-reference', ['make', '-C', PORT, 'PROGRAM=renderer_tables', 'TARGET=native',
      'WAD=' + source, 'WAD_MODE=cooked', 'CPPFLAGS=-DDOOM_RENDERER_REFERENCE=1',
      'RENDERER_BF_SMOKE=' + Number(tiny), 'BUILD=' + referenceBuild, 'all'], out, 900);
    const referenceBinary = path.join(referenceBuild, 'native/renderer_tables/renderer_tables');
    const referenceOutput = run('frozen-reference', [referenceBinary], out, timeout);
    const reference = tiny ? referenceOutput : JSON.parse(referenceOutput);
    if (tiny) {
      require(/^RENDERER SMOKE OK \d+ \d+\n$/.test(reference), 'incomplete renderer smoke reference');
      result.expected_output = reference;
    } else {
      require(reference.views.length === 36 && reference.zlight.length === 2048, 'incomplete reference output');
      for (let index = 0; index < 18; ++index)
        assert.deepEqual(reference.views[index], reference.views[35 - index], 'original repeated view differs');
    }
    result.checks.push({ label: 'frozen-native-reference', sha256: digest(referenceBinary),
      source_sha256: digest(path.join(PORT, 'tests/renderer_reference.inc')), views: tiny ? 1 : 36 });
    if (!tiny) {
      const packed = packRendererTables();
      comparePacked(packed.buffer, reference);
      result.checks.push({ label: 'prepared-values-vs-original-C', ...validateRendererTables(packed.buffer) });
    }

    function buildVariant(label, directory, mode, target) {
      run('build-' + label, ['make', '-C', PORT, 'PROGRAM=renderer_tables', 'TARGET=' + target,
        'WAD=' + source, 'WAD_MODE=' + mode, 'ASSET_VALIDATION=preflight',
        'RENDERER_BF_SMOKE=' + Number(tiny), 'BUILD=' + directory, 'all'], out, 900);
    }
    buildVariant('prepared', preparedBuild, 'cooked', 'both');
    const fallbackNative = path.join(preparedBuild, 'native/renderer_tables/renderer_tables');
    const nativeOutput = run('optimized-native-fallback', [fallbackNative], out, timeout);
    assert.deepEqual(tiny ? nativeOutput : JSON.parse(nativeOutput), reference,
      'optimized native renderer differs from the original algorithm');
    result.checks.push({ label: 'optimized-native-fallback', sha256: digest(fallbackNative), views: tiny ? 1 : 36 });

    function guest(label, directory, mode) {
      const binary = path.join(directory, 'ripple/renderer_tables/renderer_tables.bin');
      const disk = path.join(out, label + '.img'), reportPath = path.join(out, label + '.report.json');
      const asset = prepareDisk(source, disk, { wadMode: mode, manifest: path.join(directory, 'manifest.json') });
      fs.rmSync(reportPath, { force: true });
      const output = run(label, [RVM, binary, '--bank-size', BANK, '--memory', MEMORY,
        '--headless', '--clock', 'unthrottled', '--seed', 1, '--disk', disk, '--exit-code',
        '--timeout', timeout, '--max-instructions', maxInstructions, '--report', reportPath], out, timeout + 10);
      const report = json(reportPath);
      require(report.success === true && report.stop_reason === 'halted'
        && report.binary_sha256 === digest(binary) && report.disk_sha256 === asset.sha256,
        label + ': VM execution or input identity failed');
      assert.deepEqual(tiny ? output : JSON.parse(output), reference,
        label + ': renderer output differs from original C');
      require(digest(disk) === asset.sha256, label + ': input disk changed');
      result.checks.push({ label, views: tiny ? 1 : 36, binary, disk, vm_report: report });
    }
    guest('cooked-renderer', preparedBuild, 'cooked');
    if (!tiny) {
      buildVariant('fallback', fallbackBuild, 'expanded', 'ripple');
      guest('optimized-guest-fallback', fallbackBuild, 'expanded');
    }
    require(digest(source) === sourceHash, 'source WAD changed');
    result.success = true;
    console.log(tiny ? 'PASS renderer smoke: one prepared view and light tables match frozen native samples; guarded storage and repeated view reuse'
      : 'PASS renderer metadata: exact original C, packed tables, native fallback, cooked guest and guest fallback;'
        + ' all 18 views in both orders, repeated view reuse and split-bank guards');
    return result;
  } catch (error) {
    result.error = error.message;
    throw error;
  } finally { writeJSON(path.join(out, 'results.json'), result); }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const { values } = parseArgs({ options: { wad: { type: 'string', default: WAD },
      output: { type: 'string' }, timeout: { type: 'string', default: '180' },
      'max-instructions': { type: 'string', default: '2000000000' }, tiny: { type: 'boolean' }, help: { type: 'boolean' } } });
    if (values.help) console.log('Usage: node tools/renderer-metadata-validate.mjs [--wad FILE] [--output DIRECTORY]\n'
      + '  [--timeout SECONDS] [--max-instructions N] [--tiny]\n'
      + 'Checks every view/detail table against the frozen original C algorithm, with bounded native/Rust VM runs.\n'
      + '--tiny builds one prepared view with compact sampled checksums suitable for a separately bounded BF smoke test.');
    else validateRendererMetadata({ wad: values.wad, output: values.output,
      timeout: Number(values.timeout), maxInstructions: Number(values['max-instructions']), tiny: values.tiny });
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
