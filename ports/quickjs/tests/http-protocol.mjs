import { fetch, AbortController } from 'fuse:http';
let checks = 0;
function check(ok, message) { if (!ok) throw new Error(message); checks++; }
async function fails(action, pattern) {
    try { await action(); } catch (e) { check(pattern.test(String(e)), String(e)); return; }
    throw new Error('Expected ' + pattern);
}
const empty = await fetch('http://fixture.test/empty');
check(empty.status === 204 && await empty.text() === '', 'empty response');
const raw = await (await fetch('http://fixture.test/binary')).arrayBuffer();
check([...new Uint8Array(raw)].join() === '0,65,255,195,169', 'logical binary octets');
check((await (await fetch('http://fixture.test/binary')).text()) === '\0A\ufffdé', 'UTF-8 replacement and NUL');
await fails(async () => (await fetch('http://fixture.test/bad-json')).json(), /SyntaxError/);
await fails(() => fetch('http://fixture.test/failed'), /HTTP\/TLS worker failed/);
await fails(() => fetch('http://fixture.test/bad-headers'), /Invalid HTTP worker status/);
check(!(await fetch('http://fixture.test/missing')).ok, 'HTTP error status');
const redirected = await fetch('http://fixture.test/redirect');
check((await redirected.json()).url === 'http://fixture.test/echo?query=yes', 'redirect query');
const relative = await fetch('http://fixture.test/relative/file');
check((await relative.json()).url === 'http://fixture.test/echo?query=yes', 'relative parent and fragment');
check(await (await fetch('http://fixture.test/query')).text() === 'answer=42', 'query-only redirect');
await fails(() => fetch('http://fixture.test/loop'), /Too many HTTP redirects/);
await fails(() => fetch('http://fixture.test/redirect', { redirect: 'error' }), /redirect was disallowed/);
const cross = await fetch('http://fixture.test/cross', { headers: { Authorization: 'secret', Cookie: 'secret', 'Proxy-Authorization': 'secret', Accept: 'application/json' } });
const data = await cross.json();
check(data.url === 'http://other.test/echo' && Object.keys(data.headers).join() === 'accept', 'strip credentials across origins');
const direct = await (await fetch('http://fixture.test?x=y')).json();
check(direct.url === 'http://fixture.test/?x=y', 'query without slash');
const abort = new AbortController(); abort.abort();
await fails(() => fetch('http://fixture.test/', { signal: abort.signal }), /AbortError/);
await fails(() => fetch('http://fixture.test/', { maxBodyBytes: 262145 }), /RangeError/);
console.log('HTTP PROTOCOL CHECKS', checks);
