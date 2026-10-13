import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { analyzeProfile } from './benchmark.mjs';
import { audit, bitwiseSummary } from './bitwise-audit.mjs';

function fixture(t, { instructions, symbols, hits, object }) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'doom-profile-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const u32 = value => { const b = Buffer.alloc(4); b.writeUInt32LE(value); return b; };
  const header = Buffer.alloc(15); header.write('RLINK'); header.writeUInt16LE(64, 5);
  header.writeUInt32LE(instructions.length, 11);
  const words = instructions.map(inst => {
    const b = Buffer.alloc(8); b[0] = inst.opcode;
    for (let i = 1; i <= 3; ++i) b.writeUInt16LE(inst['word' + i] ?? 0, i * 2);
    return b;
  });
  const debug = Object.entries(symbols).flatMap(([name, pc]) => [u32(Buffer.byteLength(name)), Buffer.from(name), u32(pc)]);
  const binary = path.join(dir, 'program.bin'), samples = path.join(dir, 'hits.u64');
  fs.writeFileSync(binary, Buffer.concat([header, ...words, u32(0), Buffer.from('DEBUG'), u32(Object.keys(symbols).length), ...debug]));
  const counts = Buffer.alloc(hits.length * 8);
  hits.forEach((n, i) => counts.writeBigUInt64LE(BigInt(n), i * 8));
  fs.writeFileSync(samples, counts);
  if (object) fs.writeFileSync(path.join(dir, 'test.pobj'), JSON.stringify(object));
  return { binary, samples, dir };
}

const instruction = opcode => ({ opcode, word0: 0, word1: 0, word2: 0, word3: 0 });
const label = pc => ({ absolute_address: pc * 4 });

test('small RCC objects can have independently relocated and discarded functions', t => {
  const and = instruction(11), or = instruction(12), xor = instruction(13), ret = instruction(20), nop = instruction(66);
  const object = { instructions: [and, ret, xor, ret, or, ret],
    labels: { first: label(0), discarded: label(2), last: label(4) },
    rcc: { code: ['first', 'discarded', 'last'] } };
  const { binary, samples, dir } = fixture(t, { object,
    instructions: [or, ret, nop, nop, and, ret], symbols: { first: 4, last: 0 }, hits: [5, 5, 0, 0, 7, 7] });
  const result = analyzeProfile(binary, samples, dir);
  assert.equal(result.total, 24);
  assert.equal(result.attributed, 24);
  assert.deepEqual(result.opcodes, { JALR: 12, ANDI: 7, ORI: 5 });
  assert.deepEqual(result.functions.map(fn => [fn.name, fn.start, fn.end]), [['first', 4, 6], ['last', 0, 2]]);
  assert.deepEqual(result.hot_bitwise_instructions.map(row => [row.pc, row.function, row.hits]), [[4, 'first', 7], [0, 'last', 5]]);
});

test('stale object opcodes fail explicitly instead of receiving samples', t => {
  const object = { instructions: [instruction(11)], labels: { first: label(0) }, rcc: { code: ['first'] } };
  const { binary, samples, dir } = fixture(t, { object, instructions: [instruction(12)], symbols: { first: 0 }, hits: [2] });
  assert.throws(() => analyzeProfile(binary, samples, dir), /stale object/);
});

test('operand-only changes are stale, while declared relocations are accepted', t => {
  const li = { ...instruction(14), word1: 5, word2: 7 };
  const object = { instructions: [li], labels: { first: label(0) }, rcc: { code: ['first'] } };
  const { binary, samples, dir } = fixture(t, { object,
    instructions: [{ ...li, word2: 100 }], symbols: { first: 0 }, hits: [2] });
  assert.throws(() => analyzeProfile(binary, samples, dir), /stale object/);
  object.unresolved_references = { 0: { label: 'data', ref_type: 'data' } };
  fs.writeFileSync(path.join(dir, 'test.pobj'), JSON.stringify(object));
  assert.equal(analyzeProfile(binary, samples, dir).attributed, 2);
});

test('without matching objects all opcode totals remain exact and unattributed', t => {
  const { binary, samples } = fixture(t, {
    instructions: [instruction(3), instruction(7)], symbols: { first: 0 }, hits: [11, 17] });
  const result = analyzeProfile(binary, samples);
  assert.equal(result.total, 28);
  assert.equal(result.attributed, 0);
  assert.equal(result.unattributed, 28);
  assert.deepEqual(result.opcodes, { SRL: 17, AND: 11 });
  assert.deepEqual(result.functions, []);
  assert.deepEqual(result.hot_shift_instructions.map(row => [row.pc, row.function, row.hits]), [[1, null, 17]]);
});

test('profile length and non-exact sample counts fail explicitly', t => {
  const { binary, samples } = fixture(t, { instructions: [instruction(3)], symbols: {}, hits: [1] });
  fs.writeFileSync(samples, Buffer.alloc(0));
  assert.throws(() => analyzeProfile(binary, samples), /sample count/);
  const counts = Buffer.alloc(8); counts.writeBigUInt64LE(9007199254740992n);
  fs.writeFileSync(samples, counts);
  assert.throws(() => analyzeProfile(binary, samples), /integer range/);
});

test('audit separates bitwise ops from shifts and explicitly marks missing attribution', t => {
  const object = { instructions: [instruction(11)], labels: { first: label(0) }, rcc: { code: ['first'] } };
  const { binary, samples, dir } = fixture(t, { object,
    instructions: [instruction(12), instruction(15)], symbols: { first: 0 }, hits: [11, 17] });
  assert.throws(() => audit({ binary, hits: samples, objects: dir }), /stale object/);
  const report = audit({ binary, hits: samples, objects: dir, allowUnattributed: true });
  assert.match(report.attribution_warning, /stale object/);
  assert.equal(report.bitwise_audit.bitwise, 11);
  assert.equal(report.bitwise_audit.shifts, 17);
  assert.equal(report.bitwise_audit.unattributed_bitwise, 11);
  assert.deepEqual(report.bitwise_audit.functions, []);
  assert.match(report.hits_sha256, /^[0-9a-f]{64}$/);
  assert.deepEqual(bitwiseSummary({ total: 0, opcodes: {}, functions: [] }), {
    bitwise: 0, shifts: 0, bitwise_percent: 0, attributed_bitwise: 0, unattributed_bitwise: 0, functions: [],
  });
});
