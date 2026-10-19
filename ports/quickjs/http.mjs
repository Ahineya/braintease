// SPDX-License-Identifier: MIT
// A bounded, buffered Fetch subset. TLS and HTTP framing stay in the guest's
// htget worker, allowing the JS reactor to run during DNS and TLS preparation.
import { run, decodeUTF8, monotonicNow, getenv } from 'fuse:system';
const BODY_LIMIT = 262144;
const HEADER_LIMIT = 16384;
const token = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
const redirects = new Set([301, 302, 303, 307, 308]);
function error(name, message) { const e = new Error(message); e.name = name; return e; }
function bounded(value, fallback, max, name) {
    if (value === undefined) return fallback;
    if (!Number.isInteger(value) || value < 1 || value > max) throw new RangeError(name + ' exceeds bounds');
    return value;
}

export class AbortSignal {
    #aborted = false;
    get aborted() { return this.#aborted; }
    throwIfAborted() { if (this.#aborted) throw error('AbortError', 'The operation was aborted'); }
    // Internal transition, deliberately no EventTarget or callback dispatch.
    _abort() { this.#aborted = true; }
}
export class AbortController {
    #signal = new AbortSignal();
    get signal() { return this.#signal; }
    abort() { this.#signal._abort(); }
}

export class Headers {
    #entries;
    constructor(entries = []) {
        this.#entries = [];
        const pairs = entries instanceof Headers ? entries.entries() : Array.isArray(entries) ? entries : Object.entries(entries);
        for (const pair of pairs) {
            if (!Array.isArray(pair) || pair.length !== 2) throw new TypeError('Expected header pairs');
            const [name, value] = pair;
            if (typeof name !== 'string' || !token.test(name) || typeof value !== 'string' || /[^\t\x20-\x7e]/.test(value))
                throw new TypeError('Invalid HTTP header');
            this.#entries.push([name.toLowerCase(), value.trim()]);
        }
    }
    get(name) {
        const values = this.#entries.filter(pair => pair[0] === String(name).toLowerCase()).map(pair => pair[1]);
        return values.length ? values.join(', ') : null;
    }
    has(name) { return this.get(name) !== null; }
    *entries() { for (const [name, value] of this.#entries) yield [name, value]; }
    [Symbol.iterator]() { return this.entries(); }
}

function address(input) {
    if (typeof input !== 'string' || input.length > 1279 || /[^\x21-\x7e]/.test(input))
        throw new TypeError('Expected an ASCII HTTP(S) URL up to 1279 octets; percent-encode Unicode');
    const match = /^(https?):\/\/([^/?#]+)([^#]*)/.exec(input);
    if (!match) throw new TypeError('Only http:// and https:// URLs are supported');
    const authority = /^([A-Za-z0-9_.-]+)(?::([0-9]+))?$/.exec(match[2]);
    if (!authority || authority[1].length > 253) throw new TypeError('Unsupported URL authority');
    const scheme = match[1], host = authority[1].toLowerCase();
    const port = authority[2] === undefined ? (scheme === 'https' ? 443 : 80) : Number(authority[2]);
    if (!Number.isInteger(port) || port < 1 || port > 65535) throw new TypeError('Invalid URL port');
    const origin = scheme + '://' + host + (port === (scheme === 'https' ? 443 : 80) ? '' : ':' + port);
    let path = match[3] || '/';
    if (path[0] === '?') path = '/' + path;
    const question = path.indexOf('?');
    const search = question < 0 ? '' : path.slice(question);
    const pathname = question < 0 ? path : path.slice(0, question);
    const segments = [];
    for (const part of pathname.split('/')) {
        if (part === '..') { if (segments.length > 1) segments.pop(); }
        else if (part !== '.') segments.push(part);
    }
    if (/(?:^|\/)\.\.?$/.test(pathname)) segments.push('');
    path = segments.join('/') || '/';
    path += search;
    if (path.length > 1024 || (origin + path).length > 1279) throw new RangeError('URL exceeds HTTP worker bounds');
    return { scheme, origin, path, href: origin + path };
}
function resolve(base, location) {
    if (location.startsWith('//')) return address(base.scheme + ':' + location);
    if (/^[A-Za-z][A-Za-z0-9+.-]*:/.test(location)) return address(location);
    if (location.startsWith('/')) return address(base.origin + location);
    const path = base.path.split('?')[0];
    if (location.startsWith('?')) return address(base.origin + path + location);
    if (!location || location.startsWith('#')) return address(base.href + location);
    return address(base.origin + path.slice(0, path.lastIndexOf('/') + 1) + location);
}

export class Response {
    #bytes;
    #used = false;
    constructor(bytes, status, statusText, headers, url, redirected) {
        this.#bytes = bytes;
        Object.defineProperties(this, {
            status: { value: status, enumerable: true }, statusText: { value: statusText, enumerable: true },
            ok: { value: status >= 200 && status < 300, enumerable: true },
            headers: { value: headers, enumerable: true }, url: { value: url, enumerable: true },
            redirected: { value: redirected, enumerable: true }
        });
    }
    get bodyUsed() { return this.#used; }
    async arrayBuffer() {
        if (this.#used) throw new TypeError('Response body has already been consumed');
        this.#used = true;
        const bytes = this.#bytes; this.#bytes = null; return bytes;
    }
    async text() { return decodeUTF8(await this.arrayBuffer()); }
    async json() { return JSON.parse(await this.text()); }
}
function response(buffer, url, redirected, limit) {
    const bytes = new Uint8Array(buffer);
    let end = -1;
    for (let i = 3; i < bytes.length && i < HEADER_LIMIT; i++) {
        if (bytes[i - 3] === 13 && bytes[i - 2] === 10 && bytes[i - 1] === 13 && bytes[i] === 10) { end = i + 1; break; }
    }
    if (end < 0) throw new TypeError('Incomplete or oversized HTTP response headers');
    const lines = decodeUTF8(buffer.slice(0, end - 4)).split('\r\n');
    const status = /^HTTP\/1\.[01] ([2-5][0-9]{2})(?: ([\x20-\x7e]*))?$/.exec(lines.shift());
    if (!status) throw new TypeError('Invalid HTTP worker status');
    const pairs = lines.map(line => {
        const colon = line.indexOf(':');
        if (colon <= 0) throw new TypeError('Invalid HTTP worker header');
        return [line.slice(0, colon), line.slice(colon + 1)];
    });
    if (bytes.length - end > limit) throw new RangeError('Response body exceeds maxBodyBytes');
    const headers = new Headers(pairs);
    // htget owns dechunking and length validation; require its successful exit
    // before interpreting its buffered output, including partial TLS failures.
    return new Response(buffer.slice(end), Number(status[1]), status[2] || '', headers, url, redirected);
}

export async function fetch(input, options = {}) {
    let url = address(input);
    if (options.method !== undefined && options.method !== 'GET') throw new TypeError('Only GET is supported');
    if (options.body !== undefined) throw new TypeError('Request bodies are not supported');
    const signal = options.signal;
    if (signal !== undefined && !(signal instanceof AbortSignal)) throw new TypeError('Expected an AbortSignal');
    const timeout = bounded(options.timeoutMs, 30000, 2147483647, 'timeoutMs');
    const limit = bounded(options.maxBodyBytes, BODY_LIMIT, BODY_LIMIT, 'maxBodyBytes');
    const mode = options.redirect === undefined ? 'follow' : options.redirect;
    if (!['follow', 'manual', 'error'].includes(mode)) throw new TypeError('Invalid redirect mode');
    let headers = new Headers(options.headers);
    let pairs = [...headers];
    if (pairs.length > 4 || pairs.some(([name, value]) => name.length + value.length + 2 > 1024 ||
        ['host', 'content-length', 'transfer-encoding', 'connection', 'accept-encoding'].includes(name)))
        throw new RangeError('Request headers exceed HTTP worker bounds');
    const deadline = monotonicNow() + timeout;
    for (let hop = 0; ; hop++) {
        if (signal) signal.throwIfAborted();
        const remaining = deadline - monotonicNow();
        if (remaining <= 0) throw error('TimeoutError', 'HTTP request deadline exceeded');
        const args = ['--response'];
        for (const [name, value] of pairs) args.push('--header', name + ': ' + value);
        args.push(url.href, '-');
        let result;
        try {
            result = await run(getenv('JS_HTTP_HELPER') || '/bin/htget', args, { timeoutMs: remaining,
                maxOutputBytes: HEADER_LIMIT + limit, binary: true, signal });
        } catch (e) {
            if (e.name === 'AbortError') throw e;
            if (monotonicNow() >= deadline) throw error('TimeoutError', 'HTTP request deadline exceeded');
            throw e;
        }
        if (signal) signal.throwIfAborted();
        if (result.code !== 0 || result.signal !== null)
            throw new TypeError('HTTP/TLS worker failed: ' + result.stderr.trim().slice(0, 1024));
        const reply = response(result.stdout, url.href, hop !== 0, limit);
        if (!redirects.has(reply.status) || mode === 'manual') return reply;
        if (mode === 'error') throw new TypeError('HTTP redirect was disallowed');
        if (hop === 5) throw new TypeError('Too many HTTP redirects');
        const location = reply.headers.get('location');
        if (location === null) throw new TypeError('Redirect has no Location');
        const next = resolve(url, location);
        if (url.scheme === 'https' && next.scheme !== 'https') throw new TypeError('HTTPS downgrade is disabled');
        if (url.origin !== next.origin) pairs = pairs.filter(([name]) =>
            !['authorization', 'cookie', 'proxy-authorization'].includes(name));
        url = next;
    }
}
