import { fetch, Headers, AbortController } from 'fuse:http';
import { readTextFile, writeTextFile, run, sleep } from 'fuse:system';
const root = scriptArgs[1];
let checks = 0;
function check(ok, message) { if (!ok) throw new Error(message); checks++; }
async function rejects(action, pattern) {
    try { await action(); } catch (e) { check(pattern.test(String(e)), 'expected ' + pattern + ', got ' + e); return; }
    throw new Error('Expected failure: ' + pattern);
}
const before = [];
let pending = true, timer;
function tick() { if (pending) { before.push('tick'); timer = setTimeout(tick, 100); } }
timer = setTimeout(tick, 100);
let reply;
try { reply = await fetch('https://tls.test/chunked', { timeoutMs: 120000, headers: { Accept: 'application/json' } }); }
finally { pending = false; clearTimeout(timer); }
check(before.length > 0, 'timer during TLS request');
check(reply.ok && reply.status === 200 && reply.url === 'https://tls.test/chunked' && !reply.redirected, 'response metadata');
check(reply.headers.get('CONTENT-TYPE') === 'application/json', 'case insensitive headers');
check(!reply.bodyUsed && reply.headers.has('content-type'), 'unconsumed body');
const data = await reply.json();
check(data.message === 'Привет 🌍' && data.padding.length === 70000, 'Unicode JSON beyond 64 KiB');
check(reply.bodyUsed, 'body consumed');
await rejects(() => reply.text(), /already been consumed/);
await writeTextFile(root + '/cache.json', JSON.stringify(data));
check(JSON.parse(await readTextFile(root + '/cache.json')).message === data.message, 'cache UTF-8 round trip');
const viewed = await run('/bin/cat', [root + '/cache.json'], { maxOutputBytes: 262144 });
check(viewed.code === 0 && JSON.parse(viewed.stdout).message === data.message, 'launch viewer');
const manual = await fetch('https://tls.test/redirect', { redirect: 'manual', timeoutMs: 120000 });
check(manual.status === 302 && manual.headers.get('location') === '/binary', 'manual redirect');
const followed = await fetch('https://tls.test/redirect', { timeoutMs: 120000 });
check(followed.redirected && followed.url === 'https://tls.test/binary' && (await followed.json()).message === data.message, 'follow redirect');
await rejects(() => fetch('https://tls.test/downgrade', { timeoutMs: 120000 }), /downgrade/);
await rejects(() => fetch('https://tls.test:445/binary', { timeoutMs: 120000 }), /HTTP\/TLS worker failed/);
await rejects(() => fetch('https://wrong.test/binary', { timeoutMs: 120000 }), /HTTP\/TLS worker failed/);
await rejects(() => fetch('https://tls.test/truncated', { timeoutMs: 120000 }), /HTTP\/TLS worker failed/);
const notfound = await fetch('http://tls.test/notfound');
check(!notfound.ok && notfound.status === 404 && (await notfound.text()).includes('not found'), 'HTTP errors are responses');
await rejects(() => fetch('http://tls.test/slow', { timeoutMs: 500 }), /TimeoutError/);
const controller = new AbortController();
const waiting = fetch('http://tls.test/slow', { signal: controller.signal, timeoutMs: 30000 });
timer = setTimeout(() => controller.abort(), 1500);
try { await rejects(() => waiting, /AbortError/); } finally { clearTimeout(timer); }
const aborted = new AbortController(); aborted.abort();
await rejects(() => fetch('https://tls.test/binary', { signal: aborted.signal }), /AbortError/);
await rejects(() => fetch('http://tls.test/oversize', { maxBodyBytes: 32 }), /maxBodyBytes|too large|large|size/i);
await rejects(() => fetch('file:///etc/passwd'), /HTTP\(S\)|http:\/\//);
await rejects(() => fetch('http://user:pass@tls.test/'), /authority/);
await rejects(() => fetch('http://tls.test/', { headers: { Host: 'other.test' } }), /bounds/);
await rejects(() => fetch('http://tls.test/', { headers: { X: 'bad\r\nvalue' } }), /header/);
await rejects(() => fetch('http://tls.test/', { method: 'POST' }), /GET/);
check(new Headers([['X-Test', 'one'], ['x-test', 'two']]).get('x-test') === 'one, two', 'repeated headers');
await sleep(0);
console.log('HTTP CHECKS', checks, 'PROGRESS TICKS', before.length);
