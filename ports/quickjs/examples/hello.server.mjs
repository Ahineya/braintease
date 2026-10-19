// Run: js --server hello.server.mjs [port]
// Rift recognizes *.server.mjs: F5 runs it in server mode, Ctrl-C stops it.
import { createApp, text, json } from 'fuse:http-server';
import { sleep, monotonicNow } from 'fuse:system';
const app = createApp();
const started = monotonicNow();
app.use(async (request, next) => {
    console.log(request.method, request.url);
    return next();
});
app.get('/', () => text('Hello from JavaScript on Fuse-X / Riplix!\nTry /hello/world, /api/status and /api/slow.\n'));
app.get('/hello/:name', request => json({ hello: request.params.name, message: 'Привет 🌍' }));
app.get('/api/status', () => json({ ready: true, uptimeMs: monotonicNow() - started }));
app.get('/api/slow', async () => {
    await sleep(1000);
    return json({ message: 'Other connections and timers kept running.' });
});
app.post('/api/echo', async request => json({ received: await request.json() }));
const server = app.listen({ port: Number(scriptArgs[1] || 8080) });
console.log('Listening on', server.address.hostname + ':' + server.address.port, '— Ctrl-C stops the server');
await server.closed;
