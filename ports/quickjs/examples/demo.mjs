import {sum} from './numbers.mjs';

const buffer = new ArrayBuffer(16);
const view = new DataView(buffer);
view.setUint32(1, 0x12345678, true);
console.log('Bytes:', Array.from(new Uint8Array(buffer)).join(' '));
console.log('Sum:', sum([40000, 30000, 42]));
console.log('BigInt:', 2n ** 100n);
console.log('Await:', await Promise.resolve('ready'));
