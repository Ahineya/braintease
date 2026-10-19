// SPDX-License-Identifier: MIT
import { listen } from 'fuse:net';
import { encodeUTF8, decodeUTF8, monotonicNow } from 'fuse:system';
import { Headers, AbortController } from 'fuse:http';
const HEADER_LIMIT = 16384, BODY_LIMIT = 65536;
const token = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
const reasons = { 200: 'OK', 201: 'Created', 204: 'No Content', 304: 'Not Modified',
    400: 'Bad Request', 404: 'Not Found', 405: 'Method Not Allowed', 408: 'Request Timeout',
    413: 'Content Too Large', 417: 'Expectation Failed', 431: 'Request Header Fields Too Large',
    500: 'Internal Server Error', 501: 'Not Implemented', 503: 'Service Unavailable' };
class HTTPError extends Error {
    constructor(status) { super(reasons[status]); this.status = status; }
}
function bound(value, fallback, max, name) {
    if (value === undefined) return fallback;
    if (!Number.isInteger(value) || value < 1 || value > max) throw new RangeError(name + ' exceeds bounds');
    return value;
}
function remaining(deadline) {
    const n = deadline - monotonicNow();
    if (n <= 0) throw new HTTPError(408);
    return n;
}
class Query {
    #pairs = [];
    constructor(source) {
        if (!source) return;
        try {
            for (const pair of source.split('&')) {
                const equal = pair.indexOf('=');
                const decode = text => decodeURIComponent(text.replace(/\+/g, ' '));
                this.#pairs.push([decode(equal < 0 ? pair : pair.slice(0, equal)), decode(equal < 0 ? '' : pair.slice(equal + 1))]);
            }
        } catch { throw new HTTPError(400); }
    }
    get(name) { const pair = this.#pairs.find(pair => pair[0] === name); return pair ? pair[1] : null; }
    getAll(name) { return this.#pairs.filter(pair => pair[0] === name).map(pair => pair[1]); }
    has(name) { return this.get(name) !== null; }
    *entries() { for (const [name, value] of this.#pairs) yield [name, value]; }
    [Symbol.iterator]() { return this.entries(); }
}
class Request {
    #body; #used = false;
    constructor(method, url, headers, body, signal) {
        this.method = method; this.url = url; this.headers = headers; this.signal = signal;
        const question = url.indexOf('?');
        this.path = question < 0 ? url : url.slice(0, question);
        this.query = new Query(question < 0 ? '' : url.slice(question + 1));
        this.params = Object.create(null); this.#body = body;
    }
    get bodyUsed() { return this.#used; }
    async arrayBuffer() {
        if (this.#used) throw new TypeError('Request body has already been consumed');
        this.#used = true; const body = this.#body; this.#body = null; return body;
    }
    async text() { return decodeUTF8(await this.arrayBuffer()); }
    async json() {
        const source = await this.text();
        try { return JSON.parse(source); } catch { throw new HTTPError(400); }
    }
}
class Reply {
    constructor(body, options, contentType) {
        this.status = options.status === undefined ? 200 : options.status;
        if (!Number.isInteger(this.status) || this.status < 200 || this.status > 599) throw new RangeError('Invalid response status');
        this.headers = new Headers(options.headers);
        this.body = typeof body === 'string' ? encodeUTF8(body) : body;
        if (!(this.body instanceof ArrayBuffer)) throw new TypeError('Response requires a string or ArrayBuffer');
        if (this.body.byteLength > BODY_LIMIT) throw new RangeError('Response exceeds 64 KiB');
        if ([204, 304].includes(this.status) && this.body.byteLength) throw new TypeError('This status requires an empty body');
        this.contentType = contentType;
    }
}
export function text(body, options = {}) { return new Reply(body, options, 'text/plain; charset=utf-8'); }
export function json(value, options = {}) {
    const body = JSON.stringify(value);
    if (body === undefined) throw new TypeError('Value cannot be represented as JSON');
    return new Reply(body, options, 'application/json; charset=utf-8');
}

async function readRequest(socket, deadline, signal, bodyLimit) {
    const header = new Uint8Array(HEADER_LIMIT);
    let used = 0, match = 0, tail = new Uint8Array(0), complete = false;
    const ending = [13, 10, 13, 10];
    while (!complete) {
        const buffer = await socket.read(2048, { timeoutMs: remaining(deadline), signal });
        if (buffer === null) { if (!used) return null; throw new HTTPError(400); }
        const part = new Uint8Array(buffer);
        let n = 0;
        for (; n < part.length; n++) {
            if (used + n === HEADER_LIMIT) throw new HTTPError(431);
            const ch = part[n];
            if (ch !== 9 && ch !== 13 && ch !== 10 && (ch < 32 || ch > 126)) throw new HTTPError(400);
            match = ch === ending[match] ? match + 1 : ch === 13 ? 1 : 0;
            if (match === 4) { n++; complete = true; break; }
        }
        header.set(part.subarray(0, n), used); used += n;
        if (complete) tail = part.subarray(n);
    }
    const lines = decodeUTF8(header.buffer.slice(0, used - 4)).split('\r\n');
    if (lines.length > 65 || lines.some(line => line.length > 1024)) throw new HTTPError(431);
    const first = /^([A-Z]+) (\/[^\x00-\x20\x7f#]*) HTTP\/1\.[01]$/.exec(lines.shift());
    if (!first || first[1].length > 32) throw new HTTPError(400);
    try { decodeURIComponent(first[2]); } catch { throw new HTTPError(400); }
    const pairs = lines.map(line => {
        const colon = line.indexOf(':');
        if (colon < 1 || !token.test(line.slice(0, colon))) throw new HTTPError(400);
        return [line.slice(0, colon).toLowerCase(), line.slice(colon + 1).trim()];
    });
    let headers;
    try { headers = new Headers(pairs); } catch { throw new HTTPError(400); }
    const count = name => pairs.filter(pair => pair[0] === name).length;
    if (count('host') > 1 || (first[0].endsWith('1.1') && (!headers.get('host') || count('host') !== 1))) throw new HTTPError(400);
    if (count('content-length') > 1) throw new HTTPError(400);
    if (headers.has('transfer-encoding')) throw new HTTPError(headers.has('content-length') ? 400 : 501);
    if (headers.has('expect')) throw new HTTPError(417);
    const length = headers.get('content-length');
    if (length !== null && !/^[0-9]+$/.test(length)) throw new HTTPError(400);
    const size = length === null ? 0 : Number(length);
    if (!Number.isSafeInteger(size) || size > bodyLimit) throw new HTTPError(413);
    const body = new Uint8Array(size);
    let at = Math.min(size, tail.length); body.set(tail.subarray(0, at));
    while (at < size) {
        const buffer = await socket.read(Math.min(2048, size - at), { timeoutMs: remaining(deadline), signal });
        if (buffer === null) throw new HTTPError(400);
        body.set(new Uint8Array(buffer), at); at += buffer.byteLength;
    }
    return new Request(first[1], first[2], headers, body.buffer, signal);
}
async function writeReply(socket, reply, head, deadline, commit = () => {}) {
    if (!(reply instanceof Reply)) throw new TypeError('Handler must return text() or json()');
    let header = 'HTTP/1.1 ' + reply.status + ' ' + (reasons[reply.status] || 'Response') + '\r\nConnection: close\r\n';
    if (![204, 304].includes(reply.status)) header += 'Content-Length: ' + reply.body.byteLength + '\r\n';
    if (!reply.headers.has('content-type')) header += 'Content-Type: ' + reply.contentType + '\r\n';
    for (const [name, value] of reply.headers) {
        if (['content-length', 'transfer-encoding', 'connection'].includes(name)) throw new TypeError('Response framing headers are managed by the server');
        header += name + ': ' + value + '\r\n';
    }
    header += '\r\n';
    if (header.length > HEADER_LIMIT) throw new RangeError('Response headers exceed bounds');
    commit();
    await socket.write(header, { timeoutMs: remaining(deadline) });
    if (!head && reply.body.byteLength) await socket.write(reply.body, { timeoutMs: remaining(deadline) });
}
class Server {
    #listener; #handler; #options; #active = new Map(); #stopping = false;
    constructor(handler, options) {
        if (typeof handler !== 'function') throw new TypeError('Expected an HTTP handler');
        this.#handler = handler;
        this.#options = {
            maxConnections: bound(options.maxConnections, 2, 4, 'maxConnections'),
            timeoutMs: bound(options.timeoutMs, 30000, 2147483647, 'timeoutMs'),
            maxBodyBytes: bound(options.maxBodyBytes, BODY_LIMIT, BODY_LIMIT, 'maxBodyBytes')
        };
        if (options.signal !== undefined) throw new TypeError('Stop the server with close(); operation signals belong to individual sockets');
        this.#listener = listen(options);
        Object.defineProperty(this, 'address', { value: Object.freeze(this.#listener.address), enumerable: true });
        Object.defineProperty(this, 'closed', { value: this.#loop(), enumerable: true });
    }
    close({ force = false } = {}) {
        this.#stopping = true; this.#listener.close();
        if (force) for (const socket of this.#active.keys()) socket.close();
    }
    async #exchange(socket) {
        const deadline = monotonicNow() + this.#options.timeoutMs;
        const controller = new AbortController();
        let timer, request, committed = false;
        try {
            request = await readRequest(socket, deadline, controller.signal, this.#options.maxBodyBytes);
            if (!request) return;
            const timeout = new Promise((resolve, reject) => {
                timer = setTimeout(() => { controller.abort(); reject(new HTTPError(408)); }, remaining(deadline));
            });
            let reply;
            try { reply = await Promise.race([Promise.resolve().then(() => this.#handler(request)), timeout]); }
            finally { clearTimeout(timer); }
            await writeReply(socket, reply, request.method === 'HEAD', deadline, () => { committed = true; });
        } catch (e) {
            if (!(e instanceof HTTPError) && !(e && e.errno !== undefined)) console.error('HTTP handler:', String(e), e && e.stack || '');
            try { if (!committed) await writeReply(socket, text((e instanceof HTTPError ? e.message : reasons[500]) + '\n',
                { status: e instanceof HTTPError ? e.status : 500 }), request && request.method === 'HEAD', monotonicNow() + 1000); }
            catch { /* Disconnects and I/O deadlines already close the socket. */ }
        } finally { clearTimeout(timer); controller.abort(); socket.close(); }
    }
    async #loop() {
        try {
            while (!this.#stopping) {
                if (this.#active.size >= this.#options.maxConnections) {
                    await Promise.race(this.#active.values()); continue;
                }
                let socket;
                try { socket = await this.#listener.accept({ timeoutMs: 2147483647, signal: undefined }); }
                catch (e) {
                    if (this.#stopping) break;
                    if (e.name === 'AbortError') { this.close({ force: true }); break; }
                    throw e;
                }
                if (this.#stopping) { socket.close(); break; }
                const task = this.#exchange(socket).finally(() => this.#active.delete(socket));
                this.#active.set(socket, task);
            }
        } finally {
            this.#stopping = true; this.#listener.close();
            await Promise.allSettled(this.#active.values());
        }
    }
}
export function serve(handler, options = {}) { return new Server(handler, options); }

export function createApp() {
    const routes = [], middleware = [];
    const app = {
        use(handler) {
            if (typeof handler !== 'function') throw new TypeError('Expected middleware');
            if (middleware.length === 32) throw new RangeError('Middleware limit exceeded');
            middleware.push(handler); return app;
        },
        route(method, pattern, handler) {
            if (typeof method !== 'string' || !/^[A-Z]+$/.test(method) || typeof pattern !== 'string' || !pattern.startsWith('/') || typeof handler !== 'function')
                throw new TypeError('Invalid route');
            if (routes.length === 64) throw new RangeError('Route limit exceeded');
            const names = new Set();
            for (const part of pattern.split('/')) if (part.startsWith(':')) {
                if (!/^:[A-Za-z_][A-Za-z0-9_]*$/.test(part) || names.has(part)) throw new TypeError('Invalid or duplicate route parameter');
                names.add(part);
            }
            routes.push({ method, parts: pattern.split('/'), handler }); return app;
        },
        get(pattern, handler) { return app.route('GET', pattern, handler); },
        post(pattern, handler) { return app.route('POST', pattern, handler); },
        async handle(request) {
            const allowed = new Set();
            async function route() {
                await Promise.resolve();
                const parts = request.path.split('/').map(part => decodeURIComponent(part));
                for (const candidate of routes) {
                    if (parts.length !== candidate.parts.length) continue;
                    const params = Object.create(null);
                    let match = true;
                    for (let i = 0; i < parts.length; i++) {
                        const part = candidate.parts[i];
                        if (part.startsWith(':') && parts[i]) params[part.slice(1)] = parts[i];
                        else if (part !== parts[i]) { match = false; break; }
                    }
                    if (!match) continue;
                    if (candidate.method !== request.method && !(request.method === 'HEAD' && candidate.method === 'GET')) { allowed.add(candidate.method); if (candidate.method === 'GET') allowed.add('HEAD'); continue; }
                    request.params = params; return candidate.handler(request);
                }
                return allowed.size ? text('Method Not Allowed\n', { status: 405, headers: { Allow: [...allowed].join(', ') } }) : text('Not Found\n', { status: 404 });
            }
            async function dispatch(index) {
                // Async functions execute synchronously up to their first await.
                // Make every middleware hop a separate job: RVM has a bounded
                // native stack, even though suspended JS frames live on the heap.
                await Promise.resolve();
                if (index === middleware.length) return route();
                let called = false;
                return middleware[index](request, () => {
                    if (called) throw new Error('next() may only be called once');
                    called = true; return dispatch(index + 1);
                });
            }
            return dispatch(0);
        },
        listen(options = {}) { return serve(request => app.handle(request), options); }
    };
    return app;
}
