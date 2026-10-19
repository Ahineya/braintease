// Read-only compatibility checks shared by the page, worker and import tests.
export const MAX_DISK_BYTES = 256 * 1024 * 1024;
export function checkDiskSize(size) {
  if (!Number.isSafeInteger(size) || size < 512 || size > MAX_DISK_BYTES)
    throw Error('Choose a Ripple/FUZIX .img disk no larger than 256 MiB.');
}
export function validateDisk(buffer) {
  const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer);
  checkDiskSize(bytes.byteLength);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const u16 = at => view.getUint16(at, true), u32 = at => view.getUint32(at, true);
  const text = (at, n) => String.fromCharCode(...bytes.subarray(at, at + n));
  const invalid = message => { throw Error(message); };
  if (u16(510) !== 0xaa55) invalid('This is not a Ripple/FUZIX FAT16 disk image.');
  const bps = u16(11), spc = bytes[13], reserved = u16(14), fats = bytes[16];
  const entries = u16(17), sectors = u16(19) || u32(32), spf = u16(22);
  if (![512,1024,2048,4096].includes(bps) || !spc || spc > 128 || (spc & (spc-1)) ||
      !reserved || !entries || !spf || fats < 1 || fats > 4 || sectors*bps !== bytes.length)
    invalid('The disk has an invalid or incomplete FAT16 layout.');
  const fat = reserved*bps, fatBytes = spf*bps, root = fat + fats*fatBytes;
  const data = root + Math.ceil(entries*32/bps)*bps, clusterBytes = bps*spc;
  const clusters = Math.floor((bytes.length-data)/clusterBytes);
  if (data >= bytes.length || clusters < 4085 || clusters >= 0xffee || (clusters+2)*2 > fatBytes)
    invalid('The disk has an invalid FAT16 allocation table.');
  for (let copy=1; copy<fats; copy++)
    for (let i=0; i<fatBytes; i++)
      if (bytes[fat+i] !== bytes[fat+copy*fatBytes+i]) invalid('The disk allocation tables disagree.');
  const occupied = new Set();
  function file(name) {
    let entry;
    for (let i=0; i<entries; i++) {
      const at = root+i*32;
      if (!bytes[at]) break;
      if (bytes[at] === 0xe5 || bytes[at+11]&0x18) continue;
      if (text(at,11) === name) {
        if (entry !== undefined) invalid('The disk contains duplicate system files.');
        entry = at;
      }
    }
    if (entry === undefined) invalid('Choose a Ripple/FUZIX disk containing FUZIX.SYS and ROOTFS.IMG.');
    const size = u32(entry+28), chain = [];
    if (!size || size > bytes.length) invalid('A system file has an invalid size.');
    let cluster = u16(entry+26);
    for (let at=0; at<size; at+=clusterBytes) {
      if (cluster < 2 || cluster >= 0xfff0 || cluster >= clusters+2 || occupied.has(cluster))
        invalid('A system file has a broken or overlapping allocation chain.');
      occupied.add(cluster);
      chain.push(data+(cluster-2)*clusterBytes);
      cluster = u16(fat+cluster*2);
    }
    if (cluster < 0xfff8) invalid('A system file has an invalid allocation chain.');
    return {size, prefix(n) {
      if (size < n) invalid('A system file is truncated.');
      const result = new Uint8Array(n);
      for (let at=0; at<n;) {
        const within = at%clusterBytes, take = Math.min(n-at,clusterBytes-within);
        result.set(bytes.subarray(chain[Math.floor(at/clusterBytes)]+within,
          chain[Math.floor(at/clusterBytes)]+within+take),at);
        at += take;
      }
      return result;
    }};
  }
  const kernel = file('FUZIX   SYS'), header = kernel.prefix(24);
  const k = new DataView(header.buffer), count = k.getUint32(16,true), initialized = k.getUint32(20,true);
  if (String.fromCharCode(...header.subarray(0,4)) !== 'SYS1' ||
      k.getUint16(4,true) !== 64000 || k.getUint16(6,true) !== 1 ||
      k.getUint16(8,true) !== 3 || k.getUint16(10,true) !== 32 || !count ||
      count > 31*64000 || k.getUint32(12,true) >= count || initialized > 64000 ||
      24+count*8+initialized !== kernel.size)
    invalid('This kernel is incompatible with the browser Ripple machine.');
  const filesystem = file('ROOTFS  IMG'), prefix = filesystem.prefix(4096);
  const f = new DataView(prefix.buffer), isize = f.getUint16(2050,true), blocks = f.getUint32(2052,true);
  if (String.fromCharCode(...prefix.subarray(0,19)) !== 'RIPPLE-FUZIX-C16-V3' ||
      f.getUint16(2048,true) !== 0x7254 || isize < 3 || (isize-2)*16 > 65535 ||
      blocks <= isize || blocks*2048 !== filesystem.size)
    invalid('The browser requires a Ripple C16 V3 filesystem.');
  return {bytes:bytes.byteLength, rootBytes:filesystem.size};
}
