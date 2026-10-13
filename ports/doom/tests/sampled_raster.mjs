#!/usr/bin/env node
// Compile the actual sampled C drawers through RCC, then exercise translation
// and invisibility without booting the engine or running any Brainfuck code.
import fs from 'node:fs';
import path from 'node:path';
import { PORT, WAD, BANK, MEMORY, RVM, commands, build, run, require, mkdir,
  json, writeJSON } from '../tools/test-support.mjs';

const out = path.resolve(process.argv[2] || path.join(PORT, 'build/tests/sampled-raster'));
const wad = path.resolve(process.argv[3] || WAD);
mkdir(out);
const result = { success: false, checks: [], commands };
try {
  for (const scale of [2, 4]) {
    const directory = path.join(out, 'scale' + scale);
    const buildDir = path.join(directory, 'build');
    mkdir(directory);
    build('sampled_raster', directory, {
      target: 'both', buildDir, resolutionScale: scale, wad, wadMode: 'packed',
    });
    const expected = `SAMPLED RASTER OK scale ${scale}, 128 translated/fuzz cases, sparse cells, neighbors, bounds and cursor\n`;
    const native = run('sampled-raster-native',
      [path.join(buildDir, 'native/sampled_raster/sampled_raster')], directory, 30);
    require(native === expected, 'scale ' + scale + ': native diagnostic differs: ' + native);

    const reportPath = path.join(directory, 'guest.report.json');
    const disk = path.join(directory, 'blank-disk.img');
    fs.rmSync(reportPath, { force: true });
    fs.writeFileSync(disk, Buffer.alloc(65536));
    const guest = run('sampled-raster-ripple', [RVM,
      path.join(buildDir, 'ripple/sampled_raster/sampled_raster.bin'),
      '--bank-size', BANK, '--memory', MEMORY, '--headless', '--clock', 'unthrottled',
      '--seed', 1, '--disk', disk, '--exit-code', '--max-instructions', 2000000000,
      '--timeout', 120, '--report', reportPath], directory, 130);
    const report = json(reportPath);
    require(guest === expected, 'scale ' + scale + ': guest diagnostic differs: ' + guest);
    require(report.success === true && report.stop_reason === 'halted',
      'scale ' + scale + ': guest did not halt successfully');
    result.checks.push({ scale, output: guest, vm_report: report });
    console.log('PASS sampled raster scale ' + scale + ': native C and RCC/Rust VM');
  }
  result.success = true;
  console.log('SAMPLED RASTER CHECK OK scales 2/4; translated/fuzz drawers, sparse cells, neighbors, bounds and cursor');
} catch (error) {
  result.error = error.message;
  console.error(error.message);
  process.exitCode = 1;
} finally {
  writeJSON(path.join(out, 'results.json'), result);
}
