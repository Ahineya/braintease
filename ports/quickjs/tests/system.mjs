import { readDir, readTextFile, writeTextFile, stat, cwd, getenv, run, sleep, decodeUTF8, monotonicNow } from 'fuse:system';
const root = scriptArgs[1];
const child = scriptArgs[2];
let checks = 0;
function check(value, message) {
    if (!value) throw new Error(message);
    checks++;
}
async function rejected(task, syscall) {
    try { await task; } catch (error) {
        check(error instanceof Error && error.errno > 0 && error.syscall === syscall, 'OS error: ' + error);
        return error;
    }
    throw new Error('Expected rejection from ' + syscall);
}
function invalid(action) {
    try { action(); } catch (error) {
        check(error instanceof TypeError || error instanceof RangeError, 'argument error');
        return;
    }
    throw new Error('Expected argument error');
}
check(typeof cwd() === 'string' && cwd().startsWith('/'), 'cwd');
check(getenv('QJS_VARIABLE_THAT_DOES_NOT_EXIST') === undefined, 'missing env');
// Create the UTF-8 name through the guest API: the legacy image installer
// currently writes non-ASCII manifest names as UTF-16 cells rather than octets.
await writeTextFile(root + '/привет.txt', await readTextFile(root + '/unicode-source.txt'));
let deferred = false;
const listing = readDir(root).then(names => { check(deferred, 'deferred directory result'); return names; });
deferred = true;
const names = await listing;
check(names.includes('sample.json') && names.includes('привет.txt'), 'directory UTF-8 names');
check(!names.includes('.') && !names.includes('..'), 'directory excludes dot entries');
const [source, note, info] = await Promise.all([
    readTextFile(root + '/sample.json'), readTextFile(root + '/привет.txt'), stat(root + '/sample.json')
]);
const config = JSON.parse(source);
check(config.message === 'Привет 🌍', 'JSON UTF-8');
check(note === 'Καλημέρα! こんにちは! Привет 🌍\n', 'file UTF-8');
check(info.isFile && !info.isDirectory && info.size > 0, 'stat file');
check((await stat(root)).isDirectory, 'stat directory');
check((await readTextFile(root + '/nul.txt')) === 'A\0B', 'length-aware NUL text');
let beforeRead = false;
const large = readTextFile(root + '/bank.txt');
setTimeout(() => { beforeRead = true; }, 0);
check((await large).length === 70000 && beforeRead, 'banked read shares timer loop');
await writeTextFile(root + '/report.txt', note + '\0END');
check((await readTextFile(root + '/report.txt')) === note + '\0END', 'Unicode/NUL write round trip');
await writeTextFile(root + '/report.txt', '');
check((await stat(root + '/report.txt')).size === 0, 'empty write truncates');
const order = [];
const cancelled = setTimeout(() => { throw new Error('cancelled timer fired'); }, 0);
clearTimeout(cancelled);
setTimeout((a, b) => order.push(a + b), 0, 'timer', ' args');
Promise.resolve().then(() => order.push('microtask'));
order.push('sync');
// Both zero-delay tasks are due together, even on a fast host. Reusing a
// request slot must not let the later sleep overtake the earlier timer.
await sleep(0);
check(order.join(',') === 'sync,microtask,timer args', 'timer and microtask ordering');
let count = 0, observed = -1;
function chain() { count++; if (count < 150) return Promise.resolve().then(chain); }
setTimeout(() => { observed = count; }, 0);
await chain();
await sleep(0);
check(observed >= 0 && observed < 150, 'microtasks yield to timers');
let timerDuringChild = false, childDone = false;
const running = run(child, ['wait']).then(result => { childDone = true; return result; });
setTimeout(() => { timerDuringChild = !childDone; }, 10);
const result = await running;
check(timerDuringChild, 'timer fired while child was running');
check(result.code === 7 && result.signal === null, 'child nonzero exit status');
check(result.stdout === 'BEFORE\nAFTER\n' && result.stderr === '', 'captured output: ' + JSON.stringify(result));
const duplex = await run(child, ['duplex']);
check(duplex.code === 0 && duplex.stdout.length === 32768 && duplex.stderr.length === 32768, 'drain both pipes beyond capacity');
const concurrent = await Promise.all([run(child, ['wait']), run(child, ['other child'])]);
check(concurrent[0].code === 7 && concurrent[1].stdout === 'other child\n', 'independent concurrent children');
const args = await run(child, ['one two', '$(no-shell); *', 'Привет']);
check(args.stdout === 'one two\n$(no-shell); *\nПривет\n', 'literal arguments and Unicode');
const raw = await run(child, ['Привет'], { binary: true, maxOutputBytes: 32 });
check(raw.stdout instanceof ArrayBuffer && raw.stdout.byteLength === 13 && decodeUTF8(raw.stdout) === 'Привет\n', 'binary output preserves UTF-8 octets');
check(decodeUTF8(new ArrayBuffer(0)) === '', 'empty byte buffer');
const time = monotonicNow(); await sleep(0); check(monotonicNow() >= time, 'monotonic clock');
const signal = { aborted: false };
const cancellable = run(child, ['hang', root + '/abort.pid'], { signal });
let abortTimer = setTimeout(() => { signal.aborted = true; }, 1500);
const abortError = await rejected(cancellable, 'run'); clearTimeout(abortTimer);
check(abortError.name === 'AbortError' && (await run(child, ['gone', root + '/abort.pid'])).code === 0, 'abort kills and reaps child');
await rejected(run(child, ['wait'], { maxOutputBytes: 5 }), 'run');
const signalled = await run(child, ['signal']);
check(signalled.code === null && signalled.signal > 0, 'signal exit status');
await rejected(readTextFile(root + '/absent'), 'open');
await rejected(readDir(root + '/absent'), 'opendir');
await rejected(stat(root + '/absent'), 'stat');
await rejected(readTextFile(root), 'text file');
await rejected(run(root + '/absent', []), 'execvp');
await rejected(run(root + '/not-executable', []), 'execvp');
await rejected(run(child, ['overflow']), 'run');
await rejected(run(child, ['hang'], { timeoutMs: 40 }), 'run');
check((await run(child, ['after failures'])).stdout === 'after failures\n', 'child cleanup and recovery');
for (let i = 0; i < 40; i++) {
    await readDir(root);
    await readTextFile(root + '/sample.json');
}
check(true, 'repeated operations release descriptors and task slots');
invalid(() => readTextFile('x\0hidden'));
invalid(() => readDir(42));
invalid(() => run(child, ['x\0hidden']));
invalid(() => run(child, 42));
invalid(() => run(child, [], { maxOutputBytes: 1048577 }));
invalid(() => run(child, [], { maxOutputBytes: 1.5 }));
invalid(() => run(child, [], { signal: 42 }));
invalid(() => sleep(-1));
invalid(() => sleep(NaN));
invalid(() => setTimeout(42, 0));
const timers = [];
for (let i = 0; i < 32; i++) timers.push(setTimeout(() => {}, 60000));
invalid(() => setTimeout(() => {}, 60000));
timers.forEach(clearTimeout);
console.log('SYSTEM CHECKS', checks);
