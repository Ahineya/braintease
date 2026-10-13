#!/usr/bin/env node
// Independent full-resolution oracle for the sparse UI drawing paths.
import fs from 'node:fs';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const port = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const out = path.resolve(process.argv[2] || path.join(port, 'build/tests/ui-sampling'));
fs.mkdirSync(out, { recursive: true });
function run(command, args) {
  const result = spawnSync(command, args, { encoding: 'utf8', timeout: 30000 });
  if (result.error || result.status !== 0)
    throw new Error([command, ...args].join(' ') + '\n' + (result.error?.message || result.stderr));
}
let reference;
let frames;
const cc = process.env.CC || 'clang';
const sdk = spawnSync(process.execPath, [path.join(port, 'tools/build-support.mjs'), 'native-flags', cc],
  { encoding: 'utf8', timeout: 30000 });
if (sdk.error || sdk.status !== 0) throw new Error(sdk.error?.message || sdk.stderr);
const nativeFlags = sdk.stdout.trim();
const sdkArgs = nativeFlags ? ['-isysroot', JSON.parse(nativeFlags.replace(/^-isysroot /, ''))] : [];
for (const scale of [1, 2, 4]) {
  const exe = path.join(out, 'ui-sampling-' + scale), raw = exe + '.raw';
  run(cc, ['-std=c99', '-O2', '-fwrapv', ...sdkArgs, '-ffunction-sections', '-fdata-sections',
    process.platform === 'darwin' ? '-Wl,-dead_strip' : '-Wl,--gc-sections',
    '-DDOOM_PORT', '-DDOOM_RESOLUTION_SCALE=' + scale,
    '-I' + path.join(port, 'engine'), '-I' + path.join(port, 'src'),
    path.join(port, 'tests/ui_sampling.c'), path.join(port, 'engine/v_video.c'),
    path.join(port, 'engine/f_finale.c'), path.join(port, 'engine/r_draw.c'), '-lm', '-o', exe]);
  run(exe, [raw]);
  const actual = fs.readFileSync(raw);
  if (scale === 1) {
    reference = actual;
    frames = actual.length / 64000;
    if (!Number.isInteger(frames) || frames < 160) throw new Error('Missing oracle cases');
  } else {
    if (actual.length !== reference.length) throw new Error('Case count differs');
    for (let frame = 0; frame < frames; ++frame)
      for (let y = 0; y < 200; y += scale)
        for (let x = 0; x < 320; x += scale) {
          const offset = frame * 64000 + y * 320 + x;
          if (actual[offset] !== reference[offset])
            throw new Error(`Scale ${scale}, case ${frame}, pixel ${x},${y}: ${actual[offset]} != ${reference[offset]}`);
        }
    const rejected = spawnSync(exe, [raw, 'unaligned'], { encoding: 'utf8', timeout: 30000 });
    if (rejected.error || rejected.status !== 89 || !rejected.stderr.includes('unaligned sampled screen copy'))
      throw new Error('Unaligned sparse copy was not rejected');
    for (const name of ['column-x', 'column-y', 'column-low', 'translated-y', 'translated-low',
      'fuzz-x', 'fuzz-low', 'span-x', 'span-y', 'span-low', 'span-order']) {
      const guarded = spawnSync(exe, [raw, 'guard-' + name], { encoding: 'utf8', timeout: 30000 });
      if (guarded.error || guarded.status !== 89 || !guarded.stderr.includes('R_DrawSampled'))
        throw new Error('Malformed raster coordinates were not rejected: ' + name);
    }
  }
}
console.log(`UI SAMPLING OK ${frames} cases at scales 1/2/4; exact retained pixels, untouched discarded cells, raster range guards`);
