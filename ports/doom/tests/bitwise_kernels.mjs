#!/usr/bin/env node
// Bounded BF microbenchmarks of the actual arithmetic/rendering leaves.
// Arguments: output directory, optional baseline source directory, optional
// candidate source directory, optional comma-separated kernel selection.
// Sources contain the .asm leaves; comparisons never change the working tree.
import fs from 'node:fs';
import path from 'node:path';
import { ROOT, PORT, run, commands, mkdir, json, require, writeJSON } from '../tools/test-support.mjs';

const out = path.resolve(process.argv[2] || path.join(PORT, 'build/tests/bitwise-kernels'));
const baseline = process.argv[3] && path.resolve(process.argv[3]);
const candidate = process.argv[4] ? path.resolve(process.argv[4]) : path.join(PORT, 'src');
const selection = process.argv[5]?.split(',');
mkdir(out);
const result = { success: false, checks: [], commands };
const entry = ['start:', '    LI SB, 1', '    LI GP, 1', '    LI SP, 1000'];
const call = name => [`    LI S2, %bank(${name})`, `    LI S3, %offset(${name})`, '    JALR RA, S2, S3'];
const set = (register, value) => `    LI ${register}, ${Number(value) & 65535}`;
const stack = values => values.flatMap((value, index) =>
  [set('T0', value), set('T1', 999 - index), '    STORE T0, SB, T1']);
const finish = ['    LI A0, 89', '    STORE A0, R0, R0', '    LI A0, 10',
  '    STORE A0, R0, R0', '    LI RV0, 0', '    HALT', 'fail:',
  '    LI A0, 78', '    STORE A0, R0, R0', '    LI RV0, 1', '    HALT'];
function fixed(name) {
  const code = [...entry];
  for (const [a, b] of [[0x12345678, 0x123456], [0xedcba988, 0x123456],
    [0x12345678, 0xffedcbaa], [0xedcba988, 0xffedcbaa], [0x12345678, 0x310000],
    [0x7fffffff, 1], [0x80000000, 1], [1, 0], [0x80000000, 0x80000000]]) {
    const signed = word => BigInt(word | 0), abs = value => value < 0n ? -value : value;
    const x = signed(a), y = signed(b);
    const expected = name === 'FixedMul' ? BigInt.asUintN(32, (x * y) >> 16n)
      : (abs(x) >> 14n) >= abs(y)
      ? ((x < 0n) !== (y < 0n) ? 0x80000000n : 0x7fffffffn)
      : BigInt.asUintN(32, (x * 65536n) / y);
    code.push(set('A0', a), set('A1', a >>> 16), set('A2', b), set('A3', b >>> 16),
      ...call(name), set('T0', Number(expected & 65535n)), '    BNE RV0, T0, fail',
      set('T0', Number(expected >> 16n)), '    BNE RV1, T0, fail');
  }
  return code;
}
function raster(span) {
  const code = [...entry, set('T0', 37), set('T1', 2000), '    STORE T0, SB, T1'];
  const stride = span ? 1 : 320;
  code.push(...stack([2000, 1, 0xba98, 0xfedc, 0x4321, 0x8765, 24]),
    set('A0', 12000), set('A1', 1), set('A2', 5000), set('A3', 1),
    ...call(span ? 'Doom_DrawSpan' : 'Doom_DrawColumn'));
  for (let i = 0; i < 24; ++i)
    code.push(set('T0', 12000 + i * stride), '    LOAD T1, SB, T0', set('T0', 37), '    BNE T1, T0, fail');
  return code;
}
function render(fill) {
  const code = [...entry, set('T0', 37), set('T1', 2000), '    STORE T0, SB, T1'];
  if (fill) code.push(...stack([2]), set('A0', 12000), set('A1', 1), set('A2', 37),
    set('A3', 159), ...call('Doom_FillSamples'));
  else code.push(...stack([2000, 1, 159, 2]), set('A0', 12000), set('A1', 1),
    set('A2', 5000), set('A3', 1), ...call('Doom_ConvertRGB565SampledRow'));
  for (let i = 0; i < 159; ++i)
    code.push(set('T0', 12000 + i * (fill ? 2 : 1)), '    LOAD T1, SB, T0',
      set('T0', 37), '    BNE T1, T0, fail');
  return code;
}
try {
  for (const [variant, sourceDir] of [...(baseline ? [['before', baseline]] : []), ['after', candidate]]) {
    for (const [name, file, caller] of [
      ['fixed-div', 'fixed_fast.asm', fixed('FixedDiv')],
      ['fixed-mul', 'fixed_fast.asm', fixed('FixedMul')],
      ['column', 'raster_fast.asm', raster(false)],
      ['span', 'raster_fast.asm', raster(true)],
      ['convert', 'render_fast.asm', render(false)],
      ['fill', 'render_fast.asm', render(true)],
    ]) {
      if (selection && !selection.includes(name)) continue;
      const directory = path.join(out, variant, name); mkdir(directory);
      const prefix = path.join(directory, name);
      fs.writeFileSync(prefix + '.asm', [...caller, ...finish].join('\n') + '\n' + fs.readFileSync(path.join(sourceDir, file), 'utf8'));
      run('build', ['env', 'BANK_SIZE=64000', 'MEMORY_BANKS=2', 'BF_HOST=1',
        path.join(ROOT, 'scripts/build-bf.sh'), prefix + '.asm', prefix], directory, 120);
      const reportPath = path.join(directory, 'report.json');
      fs.rmSync(reportPath, { force: true });
      const output = run('run', [path.join(ROOT, 'target/release/rbf'), prefix + '.bf',
        '--headless', '--timeout', 30, '--max-instructions', 20000, '--exit-code',
        '--report', reportPath], directory, 40);
      require(output === 'Y\n', variant + ' ' + name + ': kernel result mismatch');
      const report = json(reportPath);
      require(report.success === true && report.stop_reason === 'halted', variant + ' ' + name + ': did not halt');
      result.checks.push({ variant, name, report });
      console.log('PASS BF ' + variant + ' ' + name);
    }
  }
  result.success = true;
} catch (error) {
  result.error = error.message; console.error(error.message); process.exitCode = 1;
} finally { writeJSON(path.join(out, 'results.json'), result); }
