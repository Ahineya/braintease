import { listen, connect } from 'fuse:net';
import { createApp, serve, json, text } from 'fuse:http-server';
import { sleep, decodeUTF8, encodeUTF8 } from 'fuse:system';
import { AbortController } from 'fuse:http';
const hostname = scriptArgs[1], echoPort = Number(scriptArgs[2]), bind = scriptArgs[3];
let checks = 0;
function check(ok, message) { if (!ok) throw new Error(message); checks++; }
async function rejected(action, name) {
    try { await action(); } catch (e) { check(e instanceof Error && (name ? e.name === name : e.errno > 0 || e instanceof TypeError), String(e)); return; }
    throw new Error('Expected rejection');
}
// Exercise the raw reactor before exposing HTTP. Payload exceeds the guest TCP
// buffers and spans partial writes; reads run concurrently to avoid echo deadlock.
const socket = await connect({ hostname, port: echoPort });
check(socket.peer.port === echoPort && socket.address.port > 0, 'socket addresses');
const payload = new Uint8Array(32768);
for (let i = 0; i < payload.length; i++) payload[i] = i % 256;
let ticked = false;
setTimeout(() => { ticked = true; }, 0);
const receive = (async () => {
    let at = 0;
    while (at < payload.length) {
        const data = await socket.read(2048);
        check(data !== null, 'echo unexpected EOF');
        const bytes = new Uint8Array(data);
        for (let i = 0; i < bytes.length; i++) if (bytes[i] !== (at + i) % 256) throw new Error('binary echo corruption');
        at += bytes.length;
    }
})();
await Promise.all([socket.write(payload), receive]);
check(ticked, 'timer during partial TCP writes');
socket.close(); socket.close();
const temporary = listen({ hostname: bind, port: 0 });
await rejected(() => temporary.accept({ timeoutMs: 20 }));
const abort = new AbortController();
const waiting = temporary.accept({ signal: abort.signal });
abort.abort(); await rejected(() => waiting, 'AbortError');
const stale = temporary.accept(); temporary.close();
const replacement = listen({ hostname: bind, port: 0 });
await rejected(() => stale); check(replacement.address.port > 0, 'stale handle cannot close reused descriptor');
replacement.close();
await rejected(() => connect({ hostname: 'not-an-ip', port: 80 }));
// Explicit raw read cancellation closes the affected stream, not the context.
const cancelled = await connect({ hostname, port: echoPort });
const controller = new AbortController();
const reading = cancelled.read(10, { signal: controller.signal }); controller.abort();
await rejected(() => reading, 'AbortError'); cancelled.close();
console.log('RAW_NET_OK', checks);
let ticks = 0, timer;
function progress() { ticks++; timer = setTimeout(progress, 50); }
progress();
const app = createApp();
app.use(async (request, next) => {
    request.middlewareSeen = true;
    return next();
});
for (let i = 0; i < 24; i++) app.use(async (request, next) => { request.hops = (request.hops || 0) + 1; return next(); });
app.get('/hello/:name', request => json({ hello: request.params.name, unicode: 'Привет 🌍', middleware: request.middlewareSeen, hops: request.hops,
    query: request.query.get('q'), all: request.query.getAll('q') }));
app.post('/echo', async request => json(await request.json(), { status: 201 }));
app.get('/ticks', () => json({ ticks }));
app.get('/slow', async () => { await sleep(300); return json({ slow: true, ticks }); });
app.get('/timeout', async () => { await sleep(6500); return text('too late'); });
app.get('/large', () => text('x'.repeat(32768)));
app.get('/error', () => { throw null; });
app.get('/injection', () => text('bad', { headers: { 'X-Test': 'bad\r\nInjected: yes' } }));
app.get('/empty', () => text('', { status: 204 }));
app.get('/stop', () => { server.close(); return text('STOPPED'); });
const server = app.listen({ hostname: bind, port: 0, timeoutMs: 5000 });
console.log('SERVER_READY', server.address.port);
await server.closed;
clearTimeout(timer);
console.log('SERVER_CLOSED', ticks);
const forced = serve(request => {
    if (request.path === '/force') forced.close({ force: true });
    return text('force');
}, { hostname: bind, port: 0, maxConnections: 4 });
console.log('FORCE_READY', forced.address.port);
await forced.closed;
console.log('FORCE_CLOSED');
