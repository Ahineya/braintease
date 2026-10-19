// Node harness for the portable C16 Wasm target. Browser-specific services and
// event-loop integration belong to W3/W4; no guest computation runs in JS.
import fs from 'node:fs';
import { faults } from './faults.mjs';
const path = process.argv[2];
if (!path) throw Error('usage: node runner.mjs PROGRAM.wasm');
let instance;
const imports = { ripple: { write(address, count) {
    // The range is borrowed for this call. Recreate views each time, including
    // after any future memory.grow; C16 UTF-8 stores one octet per 16-bit cell.
    const memory = instance.exports.memory;
    if (!Number.isInteger(address) || !Number.isInteger(count) || address < 0 || count < 0 || address + count*2 > memory.buffer.byteLength)
        throw Error('invalid Wasm output range');
    const view = new DataView(memory.buffer, address, count*2);
    const bytes = Buffer.alloc(count);
    for (let i=0; i<count; ++i) bytes[i] = view.getUint16(i*2, true) & 255;
    let written = 0;
    while (written < bytes.length) {
        const count = fs.writeSync(1, bytes, written, bytes.length - written);
        if (count <= 0) throw Error('Wasm output made no progress');
        written += count;
    }
} } };
try {
    const module = new WebAssembly.Module(fs.readFileSync(path));
    if (WebAssembly.Module.exports(module).some(item => item.name === '__ripple_browser_abi'))
        throw Error('this application requires a browser; use rcc-wasm/browser/bridge.mjs or the browser example launcher');
    instance = new WebAssembly.Instance(module, imports);
    if (typeof instance.exports.main !== 'function') throw Error('Wasm application has no exported main');
    if (instance.exports.main.length !== 0) throw Error('the Wasm Node runner requires main(void); argc/argv are not implemented');
    const status = instance.exports.main();
    instance.exports.__ripple_flush?.();
    process.exitCode = Number(BigInt.asUintN(8, status));
} catch (error) {
    const fault = instance?.exports.__ripple_fault?.value ?? 0;
    if (fault === 7) process.exitCode = instance.exports.__ripple_exit_status.value & 255;
    else {
        console.error(`Wasm runtime error: ${faults[fault] ?? error.message}`);
        process.exitCode = 1;
    }
}
