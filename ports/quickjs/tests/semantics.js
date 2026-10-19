function check(value, label) { nativeAssert(!!value, label); }
check(40000 + 70000 === 110000, '32-bit integer arithmetic');
check((0x80000000 | 0) === -2147483648, 'signed bitwise result');
check((-1 >>> 0) === 4294967295, 'unsigned bitwise result');
check(1 / -0 === -Infinity && Object.is(-0, -0), 'negative zero');
check(Number.isNaN(0 / 0), 'NaN');
check(9007199254740991 + 1 === 9007199254740992, 'binary64 precision');
check(JSON.parse(JSON.stringify({x: [1, 2, 3]})).x[2] === 3, 'JSON');
const counter = (() => { let value = 40000; return () => ++value; })();
check(counter() === 40001 && counter() === 40002, 'closures');
class Counter { constructor(x) { this.x = x; } next() { return ++this.x; } }
check(new Counter(70000).next() === 70001, 'classes');
check(({a: {b: 42}})?.a?.b === 42, 'optional chaining');
check(Object.create({x: 42}).x === 42, 'prototype lookup');
check(12345678901234567890n + 1n === 12345678901234567891n, 'BigInt');
check(new Map([['answer', 42]]).get('answer') === 42, 'Map');
check(/\p{Letter}+/u.test('\u0416\u0438\u0437\u043d\u044c'), 'Unicode regexp');
check('\u{1f600}'.length === 2 && '\u{1f600}'.codePointAt(0) === 0x1f600,
      'UTF-16 code units and supplementary characters');
check('a\0b'.length === 3 && 'a\0b'.charCodeAt(1) === 0, 'embedded NUL');
check('e\u0301'.normalize('NFC') === '\u00e9', 'normalization');
const buffer = new ArrayBuffer(8), bytes = new Uint8Array(buffer);
const view = new DataView(buffer);
view.setUint32(1, 0x89abcdef, true);
check(bytes[1] === 0xef && bytes[4] === 0x89 &&
      view.getUint32(1, true) === 0x89abcdef, 'unaligned octet storage');
bytes[0] = 511;
check(bytes[0] === 255, 'eight-bit truncation');
check(nativeAdd(40000, 70000) === 110000, 'C callback with 32-bit arguments');
let caught = false;
try { throw new Error('expected'); } catch (error) { caught = error.message === 'expected'; }
check(caught, 'exceptions');
for (let i = 0; i < 500; ++i) {
    let a = {}, b = {a}; a.b = b;
}
nativeGC();
check(counter() === 40003, 'closure survives collection of cycles');
globalThis.asyncDone = false;
Promise.resolve(40).then(x => x + 2).then(x => {
    check(x === 42, 'Promise job queue');
    asyncDone = true;
});
