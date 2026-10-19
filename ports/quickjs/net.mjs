// SPDX-License-Identifier: MIT
import * as native from 'fuse:net-native';
import { encodeUTF8 } from 'fuse:system';
const key = {};
export class Socket {
    #id;
    constructor(id, token) { if (token !== key) throw new TypeError('Use connect() or accept()'); this.#id = id; }
    get address() { return native.address(this.#id, false); }
    get peer() { return native.address(this.#id, true); }
    read(maxBytes = 2048, options = {}) { return native.read(this.#id, maxBytes, options); }
    write(data, options = {}) {
        if (typeof data === 'string') data = encodeUTF8(data);
        else if (ArrayBuffer.isView(data)) data = data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
        return native.write(this.#id, data, options);
    }
    close() { native.close(this.#id); }
}
export class Listener {
    #id;
    constructor(id, token) { if (token !== key) throw new TypeError('Use listen()'); this.#id = id; }
    get address() { return native.address(this.#id, false); }
    async accept(options = {}) { return new Socket(await native.accept(this.#id, options), key); }
    close() { native.close(this.#id); }
}
export function listen({ hostname = '0.0.0.0', port = 0 } = {}) {
    return new Listener(native.listen(hostname, port), key);
}
export async function connect({ hostname, port, timeoutMs = 30000, signal } = {}) {
    return new Socket(await native.connect(hostname, port, { timeoutMs, signal }), key);
}
