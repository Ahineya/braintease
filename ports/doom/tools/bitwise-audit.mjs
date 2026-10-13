#!/usr/bin/env node
// Count executed ISA operations from an exact PC profile, without running Doom.
import fs from 'node:fs';
import path from 'node:path';
import { parseArgs } from 'node:util';
import { pathToFileURL } from 'node:url';
import { createHash } from 'node:crypto';
import { analyzeProfile } from './benchmark.mjs';

const BITWISE = ['AND', 'ANDI', 'OR', 'ORI', 'XOR', 'XORI'];
const SHIFTS = ['SLL', 'SLLI', 'SRL', 'SRLI'];
const sum = (opcodes, keys) => keys.reduce((n, key) => n + (opcodes[key] || 0), 0);

export function bitwiseSummary(profile) {
  const functions = profile.functions.map(fn => ({ name: fn.name, symbol: fn.symbol, object: fn.object,
    instructions: fn.instructions, bitwise: sum(fn.opcodes, BITWISE), shifts: sum(fn.opcodes, SHIFTS),
    opcodes: Object.fromEntries([...BITWISE, ...SHIFTS].map(op => [op, fn.opcodes[op] || 0])) }))
    .filter(fn => fn.bitwise || fn.shifts)
    .sort((a, b) => b.bitwise - a.bitwise || b.shifts - a.shifts || a.symbol.localeCompare(b.symbol));
  const bitwise = sum(profile.opcodes, BITWISE), shifts = sum(profile.opcodes, SHIFTS);
  return { bitwise, shifts, bitwise_percent: profile.total ? bitwise * 100 / profile.total : 0,
    attributed_bitwise: functions.reduce((n, fn) => n + fn.bitwise, 0),
    unattributed_bitwise: bitwise - functions.reduce((n, fn) => n + fn.bitwise, 0),
    functions };
}

export function audit({ binary, hits, objects = null, archives = [], allowUnattributed = false }) {
  let profile, warning = null;
  try { profile = analyzeProfile(binary, hits, objects, archives); }
  catch (error) {
    if (!allowUnattributed) throw error;
    // Stale/missing objects must never turn into guessed function boundaries.
    // Binary/hit errors still fail in the totals-only retry.
    profile = analyzeProfile(binary, hits);
    warning = error.message;
  }
  return { ...profile, hits_sha256: createHash('sha256').update(fs.readFileSync(hits)).digest('hex'),
    attribution_warning: warning, bitwise_audit: bitwiseSummary(profile),
    measurement: 'Executed ISA instructions from PC hit counts; counts do not weight Brainfuck execution cost.' };
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  try {
    const { values } = parseArgs({ options: {
      binary: { type: 'string' }, hits: { type: 'string' }, objects: { type: 'string' },
      archive: { type: 'string', multiple: true }, output: { type: 'string' },
      'allow-unattributed': { type: 'boolean', default: false }, help: { type: 'boolean' },
    } });
    if (values.help) {
      console.log('Usage: node ports/doom/tools/bitwise-audit.mjs --binary FILE --hits FRAME.hits.u64\n'
        + '  [--objects DIRECTORY] [--archive FILE ...] [--output FILE] [--allow-unattributed]\n'
        + 'Use profile_vm PC-hit files and the exact binary/objects saved for that run.\n'
        + 'Without objects, totals remain exact and all function attribution is omitted.\n'
        + 'Stale objects fail by default; --allow-unattributed emits totals with a warning.\n'
        + 'This command analyzes saved samples only; it never builds or runs the guest.');
    } else {
      if (!values.binary || !values.hits) throw new Error('--binary and --hits are required');
      const report = audit({ ...values, archives: values.archive || [], allowUnattributed: values['allow-unattributed'] });
      const output = JSON.stringify(report, null, 2) + '\n';
      if (values.output) {
        fs.mkdirSync(path.dirname(path.resolve(values.output)), { recursive: true });
        fs.writeFileSync(values.output, output);
        const bits = report.bitwise_audit;
        console.log(`${report.total.toLocaleString()} guest instructions; ${bits.bitwise.toLocaleString()} bitwise, `
          + `${bits.shifts.toLocaleString()} shifts; ${bits.unattributed_bitwise.toLocaleString()} unattributed bitwise.\n${values.output}`);
      } else process.stdout.write(output);
      if (report.attribution_warning) console.error('Attribution omitted: ' + report.attribution_warning);
    }
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
