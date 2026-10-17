// C16 V3: 2 KiB blocks; 128-octet inodes; 32-bit block references.
export const CELLS = 1024, BLOCK = CELLS * 2, INODE_CELLS = 64;
export const INODES_PER_BLOCK = CELLS / INODE_CELLS;
export const MAGIC = 0x7254, SIGNATURE = 'RIPPLE-FUZIX-C16-V3';
export const ROOT_BLOCKS = 524288; // 1 GiB physical root; outer disk is 2,047 MiB.
export const DIRECT = 17, INDIRECT = CELLS / 2;
export const SB = {isize: 1, blocks: 2, nfree: 4, free: 5, ninode: 105,
                   inode: 106, clean: 156, time: 158, tfree: 160, tinode: 162, shift: 163};
export const inodeBase = ino => (2 + Math.floor(ino / INODES_PER_BLOCK)) * CELLS +
                               (ino % INODES_PER_BLOCK) * INODE_CELLS;
export const DISK_GEOMETRY = {totalSectors: 4192256, sectorsPerFat: 256, sectorsPerCluster: 64};
export function layout(image) {
  const signature = image.toString('ascii', 0, SIGNATURE.length);
  if (signature === SIGNATURE && image.readUInt16LE(CELLS * 2) === MAGIC)
    return {version: 3, cells: INODE_CELLS, inodes: INODES_PER_BLOCK, direct: DIRECT,
      indirect: INDIRECT, step: 2, sb: SB, ptr: p => image.readUInt32LE(p * 2)};
  if (signature === 'RIPPLE-FUZIX-C16-V2' && image.readUInt16LE(CELLS * 2) === 0x7253)
    return {version: 2, cells: 32, inodes: 32, direct: 18, indirect: CELLS, step: 1,
      sb: {isize: 1, blocks: 2, nfree: 3, free: 4, ninode: 54, inode: 55, clean: 105, time: 107, tfree: 109, tinode: 110, shift: 111},
      ptr: p => image.readUInt16LE(p * 2)};
  throw Error('incompatible Ripple FUZIX root; expected C16 V2 or V3');
}
export const baseFor = (ino, format) => (2 + Math.floor(ino / format.inodes)) * CELLS +
                                      (ino % format.inodes) * format.cells;
export function blockPath(index, format = {direct: DIRECT, indirect: INDIRECT, version: 3}) {
  if (index < format.direct) return [index];
  index -= format.direct;
  let span = format.indirect;
  const levels = format.version === 2 ? 2 : 3;
  for (let depth = 1; depth <= levels; depth++, span *= format.indirect) {
    if (index >= span) { index -= span; continue; }
    const result = [format.direct + depth - 1];
    for (let divisor = span / format.indirect; divisor >= 1; divisor /= format.indirect) {
      result.push(Math.floor(index / divisor)); index %= divisor;
    }
    return result;
  }
  throw Error('file exceeds block mapping capacity');
}
