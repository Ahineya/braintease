// Appended to semantics.js on both the pristine host and real C16 guests.
{
    const b = new ArrayBuffer(32), u8 = new Uint8Array(b), d = new DataView(b);
    u8[0] = 511; u8[1] = -1;
    check(u8[0] === 255 && new Int8Array(b)[1] === -1, 'octet mask and sign');
    const u16 = new Uint16Array(b), i16 = new Int16Array(b);
    u16[1] = 0xabcd;
    check(u8[2] === 0xcd && u8[3] === 0xab && i16[1] === -21555, '16-bit cross views');
    const u32 = new Uint32Array(b), i32 = new Int32Array(b);
    u32[1] = 0x89abcdef;
    check(u8[4] === 0xef && u8[7] === 0x89 && i32[1] === -1985229329, '32-bit cross views');
    const f32 = new Float32Array(b), f64 = new Float64Array(b);
    f32[2] = 1.5;
    check(u32[2] === 0x3fc00000 && f32[2] === 1.5, 'binary32 view bits');
    f64[2] = -0;
    check(u8[23] === 128 && Object.is(f64[2], -0), 'binary64 view bits');
    const bi = new BigInt64Array(b), bu = new BigUint64Array(b);
    bi[3] = -2n;
    check(bu[3] === 18446744073709551614n && u8[24] === 254 && u8[31] === 255, '64-bit BigInt cross views');
    d.setUint32(1, 0x12345678);
    check(u8[1] === 0x12 && u8[4] === 0x78 && d.getUint32(1) === 0x12345678 &&
          d.getUint32(1, true) === 0x78563412, 'DataView unaligned endian selection');
    d.setInt8(9, 511);
    check(d.getInt8(9) === -1 && d.getUint8(9) === 255, 'DataView signed octets');
    d.setBigInt64(3, -123456789012345n, true);
    check(d.getBigInt64(3, true) === -123456789012345n, 'DataView unaligned BigInt');
    d.setFloat64(5, Math.PI, true);
    check(d.getFloat64(5, true) === Math.PI, 'DataView unaligned binary64');
    const h = new Float16Array([1.5, -0, Infinity]);
    check(h[0] === 1.5 && Object.is(h[1], -0) && h[2] === Infinity, 'binary16 conversion');
    const clamp = new Uint8ClampedArray([-2, 2.5, 3.5, 300]);
    check(clamp.join(',') === '0,2,4,255', 'clamped octet rounding');
    const a = new Uint32Array([70000, 0xffffffff, 42, 256]);
    check(a.reverse().join(',') === '256,42,4294967295,70000', 'reverse logical elements');
    a.fill(0xabcdef01, 1, 3);
    check(a[0] === 256 && a[1] === 0xabcdef01 && a[2] === a[1] && a[3] === 70000, 'fill logical elements');
    a.copyWithin(1, 0, 3);
    check(a[0] === 256 && a[1] === 256 && a[2] === 0xabcdef01 && a[3] === a[2], 'overlapping copyWithin');
    const sub = a.subarray(1, 3); sub[0] = 99;
    check(a[1] === 99 && sub.byteOffset === 4 && sub.byteLength === 8, 'subarray shares storage');
    const copy = a.slice(1, 3); copy[0] = 123;
    check(a[1] === 99 && copy[0] === 123, 'slice owns storage');
    const order = new Int16Array([32767, -32768, 256, -1, 0]);
    check(order.sort().join(',') === '-32768,-1,0,256,32767', 'signed native-free sort');
    check(order.sort((a,b) => b-a).join(',') === '32767,256,0,-1,-32768', 'callback sort copyback');
    check(new Uint16Array([65535, 0, 32768]).sort().join(',') === '0,32768,65535', 'unsigned comparator promotion');
    const floats = new Float64Array([NaN, -0, 2, 0, -1]); floats.sort();
    check(floats[0] === -1 && Object.is(floats[1], -0) && Object.is(floats[2], 0) &&
          floats[3] === 2 && Number.isNaN(floats[4]), 'float sort NaN and negative zero');
    check(floats.includes(NaN) && floats.indexOf(NaN) === -1 && floats.indexOf(0) === 1 &&
          floats.lastIndexOf(0) === 2 && !floats.includes('2'), 'typed search equality');
    const longs = new BigUint64Array([18446744073709551615n, 2n, 0n]); longs.sort();
    check(longs[2] === 18446744073709551615n && longs.includes(2n) && !longs.includes(2), 'BigInt sort and search');
    for (const T of [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array,
                     Int32Array, Uint32Array, Float16Array, Float32Array, Float64Array]) {
        const x = new T([3, 1, 2]);
        x.fill(4, 1); x.reverse(); x.sort();
        check(x.join(',') === '3,4,4' && x.indexOf(4) === 1, T.name + ' bulk methods');
    }
    const big = new ArrayBuffer(70016), tail = new DataView(big);
    // Locate a real bank edge using this allocation's guest address.
    const edge = nativeBankOffset(big);
    tail.setFloat64(edge, 1.25, true);
    check(tail.getFloat64(edge, true) === 1.25 && new Uint8Array(big)[edge + 6] === 244,
          'DataView operand crosses bank');
    const bank = new Uint32Array(big); bank[17000] = 0x89abcdef;
    check(bank[17000] === 0x89abcdef && new Uint8Array(big)[68000] === 239, 'typed element in later bank');
    let rejected = false;
    try { new DataView(b).getFloat64(31); } catch(e) { rejected = e instanceof RangeError; }
    check(rejected, 'DataView bounds exception');
    const resizable = new ArrayBuffer(16, {maxByteLength: 64});
    const tracking = new Uint32Array(resizable), fixed = new Uint32Array(resizable, 4, 2);
    tracking[1] = 70000; resizable.resize(32);
    check(tracking.length === 8 && fixed.length === 2 && tracking[1] === 70000 && tracking[7] === 0,
          'resizable growth and view tracking');
    resizable.resize(4);
    check(tracking.length === 1 && fixed.length === 0 && fixed[0] === undefined, 'resizable view out of bounds');
    const moved = resizable.transfer();
    check(resizable.byteLength === 0 && tracking.length === 0 && moved.byteLength === 4, 'transfer detaches old views');
}
