// Locate an existing FAT16 root file without changing its allocation.
export function fatFile(image, name) {
  if (image.length < 512 || image.readUInt16LE(510) !== 0xaa55) throw Error('invalid FAT16 image');
  const bps = image.readUInt16LE(11), spc = image[13], nf = image[16],
        reserved = image.readUInt16LE(14), rootEntries = image.readUInt16LE(17),
        fatBytes = image.readUInt16LE(22) * bps;
  if (![512, 1024, 2048, 4096].includes(bps) || !spc || !nf || !fatBytes)
    throw Error('invalid FAT geometry');
  const fat = reserved * bps, root = fat + nf * fatBytes,
        data = root + Math.ceil(rootEntries * 32 / bps) * bps, clusterBytes = bps * spc;
  if (data > image.length) throw Error('truncated FAT image');
  for (let n = 1; n < nf; n++)
    if (!image.subarray(fat, fat + fatBytes)
             .equals(image.subarray(fat + n * fatBytes, fat + (n + 1) * fatBytes)))
      throw Error('FAT copies differ');
  const [base, ext = ''] = name.toUpperCase().split('.'),
                     canonical = base.padEnd(8) + ext.padEnd(3);
  let found;
  for (let i = 0; i < rootEntries; i++) {
    const off = root + i * 32;
    if (!image[off]) break;
    if (image[off] === 0xe5 || image[off + 11] & 0x18) continue;
    if (image.toString('ascii', off, off + 11) === canonical) {
      if (found !== undefined) throw Error('duplicate FAT filename');
      found = off;
    }
  }
  if (found === undefined) throw Error(`${name} missing`);
  const size = image.readUInt32LE(found + 28), regions = [], visited = new Set();
  let c = image.readUInt16LE(found + 26);
  for (let off = 0; off < size; off += clusterBytes) {
    if (c < 2 || c >= 0xfff0 || c * 2 + 2 > fatBytes || visited.has(c))
      throw Error('invalid FAT chain');
    visited.add(c);
    const at = data + (c - 2) * clusterBytes, len = Math.min(clusterBytes, size - off);
    if (at + len > image.length) throw Error('FAT chain outside disk');
    regions.push({at, off, len});
    c = image.readUInt16LE(fat + c * 2);
  }
  if (size && c < 0xfff8) throw Error('FAT chain longer than file');
  const contents = Buffer.alloc(size);
  for (const {at, off, len} of regions) image.copy(contents, off, at, at + len);
  return {
    data: contents,
    replace(replacement) {
      if (replacement.length !== size) throw Error('cannot resize preallocated FAT file');
      const result = Buffer.from(image);
      for (const {at, off, len} of regions) replacement.copy(result, at, off, off + len);
      return result;
    }
  };
}
