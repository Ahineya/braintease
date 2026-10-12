#!/usr/bin/env node
// Small compiled C rendering regressions. No Doom, window, or runtime rebuild.
// Requires current release rcc, rasm, rlink, bfm, rvm and rbf binaries.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';

const args = process.argv.slice(2);
if (args.includes('--help')) {
    console.log('Usage: node scripts/test-bf-compiler.mjs [--rvm-only] [--large-banks]\n' +
        'Verify exact C RGB565 clear, pointer and clipped-rectangle frames and instruction ceilings.\n' +
        'Runs bounded BF captures sequentially unless --rvm-only is supplied.\n' +
        '--large-banks uses 64000-cell banks to exercise distant globals and stack.\n' +
        'BF_COMPILER_OUT selects the output directory (default build/bf-compiler-regression[-large]).\n' +
        'Build current release tools first; elapsed time is reported, never asserted.');
    process.exit(0);
}
assert.ok(args.every(arg => ['--rvm-only', '--large-banks'].includes(arg)), `Unknown options: ${args}`);
const rvmOnly = args.includes('--rvm-only');
const largeBanks = args.includes('--large-banks');
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const out = path.resolve(process.env.BF_COMPILER_OUT || path.join(root,
    `build/bf-compiler-regression${largeBanks ? '-large' : ''}`));
fs.mkdirSync(out, { recursive: true });
const tools = {
    rcc: path.join(root, 'target/release/rcc'),
    rvm: path.join(root, 'target/release/rvm'),
    rasm: path.join(root, 'src/ripple-asm/target/release/rasm'),
    rlink: path.join(root, 'src/ripple-asm/target/release/rlink'),
    ...(!rvmOnly && {
        bfm: path.join(root, 'src/bf-macro-expander/target/release/bfm'),
        rbf: path.join(root, 'target/release/rbf'),
    }),
};
for (const [name, executable] of Object.entries(tools)) {
    assert.ok(fs.existsSync(executable), `Build release ${name} first: ${executable}`);
}
const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const commands = [];
function run(tool, argv, log) {
    commands.push([tools[tool], ...argv]);
    const result = spawnSync(tools[tool], argv, { cwd: root, encoding: 'utf8', maxBuffer: 32 * 1024 * 1024 });
    const logPath = path.join(out, `${log}.log`);
    fs.writeFileSync(logPath, (result.stdout || '') + (result.stderr || ''));
    assert.equal(result.status, 0, `${tool} failed; see ${logPath}\n${result.error || result.stderr || ''}`);
}
const bank = largeBanks ? 64000 : 2048, memoryBanks = 3;
function assemble(source, name) {
    const object = path.join(out, `${name}.pobj`);
    run('rasm', ['assemble', source, '-o', object, '--bank-size', String(bank), '--max-immediate', '65535'], `${name}-assemble`);
    return object;
}
function compile(source, name, defines = []) {
    const assembly = path.join(out, `${name}.asm`);
    run('rcc', ['compile', source, '-o', assembly, '-I', path.join(root, 'runtime/include'),
        '--bank-size', String(bank), ...defines.flatMap(value => ['-D', value])], `${name}-compile`);
    return assemble(assembly, name);
}
const startup = assemble(path.join(root, 'runtime/crt0.asm'), 'crt0');
const libraries = ['graphics', 'mmio'].map(name => compile(path.join(root, `runtime/${name}.c`), name));
const results = [];
const cases = [4, 8].flatMap(width => ['graphics', 'direct'].map(kind => ({ width, height: 2, kind })));
cases.push({ width: 12, height: 8, kind: 'rectangles' });
for (const { width, height, kind } of cases) {
    const name = `${kind}-${width * height}`, prefix = path.join(out, name);
    const fixture = path.join(root, `rbf/tests/fixtures/rgb565-${kind}.c`);
    const objects = [startup, compile(fixture, name, [`RENDER_WIDTH=${width}`, `RENDER_PIXELS=${width * height}`]), ...(kind !== 'direct' ? libraries : [])];
    const expected = Buffer.alloc(width * height * 2);
    for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
        let color = 0x0843;
        if (kind === 'rectangles') {
            // Explicit visible regions, independent of the C clipping loops.
            color = 0;
            if (x >= 1 && x < 5 && y >= 1 && y < 4) color = 0xf800;
            if (x >= 3 && x < 9 && y >= 2 && y < 5) color = 0x07e0;
            if (x >= 10 && y >= 6) color = 0x001f;
            if (x === 11 && y === 0) color = 0xffe0;
            if (x < 2 && y === 7) color = 0x07ff;
        }
        expected.writeUInt16LE(color, (y * width + x) * 2);
    }
    fs.writeFileSync(`${prefix}.expected.rgb565`, expected);
    // Current 4x2/8x2 counts: API281/337, direct114/186. Allow allocator
    // variation while catching lost bounds/global-load optimizations (the
    // previous API counts were711/1247). Never assert wall time.
    const ceiling = kind === 'rectangles' ? 6000 : kind === 'graphics' ? (width === 4 ? 550 : 850) : (width === 4 ? 220 : 380);
    run('rlink', [...objects, '-f', 'binary', '--bank-size', String(bank), '-o', `${prefix}.bin`], `${name}-link`);
    run('rvm', [`${prefix}.bin`, '--bank-size', String(bank), '--memory', String(bank * memoryBanks),
        '--headless', '--clock', 'unthrottled', '--capture-frame', '1', '--screenshot', `${prefix}.png`, '--raw-frame', `${prefix}.rvm.rgb565`,
        '--expect-frame', `${prefix}.expected.rgb565`, '--report', `${prefix}.rvm.json`,
        '--max-instructions', String(ceiling), '--timeout', '10', '--disk', path.join(out, 'scratch-disk.img')], `${name}-rvm`);
    const vm = JSON.parse(fs.readFileSync(`${prefix}.rvm.json`));
    assert.equal(vm.success, true);
    assert.equal(vm.frame, 1);
    assert.deepEqual(fs.readFileSync(`${prefix}.rvm.rgb565`), expected);
    assert.ok(vm.instructions <= ceiling, `${name}: ${vm.instructions} > ${ceiling}`);
    const result = { name, width, height, source_sha256: hash(fixture), instruction_ceiling: ceiling,
        guest_instructions: vm.instructions, exact_pixels: true };
    if (!rvmOnly) {
        run('rlink', [...objects, '-f', 'macro', '--standalone', '--bf-host', '--bank-size', String(bank),
            '--memory-banks', String(memoryBanks), '-o', `${prefix}.bfm`], `${name}-bf-link`);
        run('bfm', ['expand', `${prefix}.bfm`, '--strip-comments', '-o', `${prefix}.bf`], `${name}-expand`);
        run('rbf', [`${prefix}.bf`, '--headless', '--clock', 'unthrottled', '--capture-frame', '1',
            '--raw-frame', `${prefix}.bf.rgb565`, '--expect-frame', `${prefix}.expected.rgb565`,
            '--report', `${prefix}.bf.json`, '--max-instructions', String(ceiling),
            '--max-dispatches', '500000000', '--timeout', '30', '--disk', path.join(out, 'scratch-disk.img')], `${name}-bf`);
        const bf = JSON.parse(fs.readFileSync(`${prefix}.bf.json`));
        assert.equal(bf.success, true);
        assert.equal(bf.frames, 1);
        assert.equal(bf.ripple_instructions, vm.instructions);
        assert.deepEqual(fs.readFileSync(`${prefix}.bf.rgb565`), expected);
        Object.assign(result, { bf_dispatches: bf.bf_dispatches, bf_execution_seconds: bf.elapsed_seconds,
            linked_instruction_slots: bf.layout.instructions, tape_cells: bf.layout.tape_cells });
    }
    results.push(result);
    console.log(`${name}: exact pixels; ${vm.instructions} guest instructions` +
        (rvmOnly ? '' : `; ${result.bf_dispatches} BF dispatches; ${result.bf_execution_seconds.toFixed(4)}s`));
}
fs.writeFileSync(path.join(out, 'summary.json'), JSON.stringify({
    bank_size: bank, memory_banks: memoryBanks, clock: 'unthrottled', capture_frame: 1,
    timing: 'Sequential BF execution, excluding parsing/build; informational only.',
    tools: Object.fromEntries(Object.entries(tools).map(([name, file]) => [name, { file, sha256: hash(file) }])),
    results, commands,
}, null, 2) + '\n');
console.log(`Compiled C rendering checks passed. Artifacts: ${out}`);
