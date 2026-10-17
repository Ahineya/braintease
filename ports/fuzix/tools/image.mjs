// Bounded-memory, copy-on-write disk images. Missing pages are sparse zeroes.
// A fork snapshots page ownership; writes never alter the source image or file.
import fs from 'node:fs';
const PAGE = 2048, ZERO = Buffer.alloc(PAGE);
export class Image {
  constructor(length, source = null, pages = new Map()) {
    if (!Number.isSafeInteger(length) || length < 0 || length > 0x100000000)
      throw Error('invalid image length');
    this.length = length; this.source = source; this.pages = pages; this.owned = new Set(); this.cache = new Map();
  }
  static from(value) {
    if (value instanceof Image) return value;
    if (!Buffer.isBuffer(value)) throw Error('expected a disk image');
    return new Image(value.length, {read: (at, n) => value.subarray(at, at + n)});
  }
  fork(length = this.length) {
    this.owned.clear();
    return new Image(length, this.source, new Map(this.pages));
  }
  page(index, write = false) {
    let page = this.pages.get(index) || this.cache.get(index);
    if (!page) {
      page = this.source?.read(index * PAGE, PAGE) || ZERO;
      if (this.source) {
        if (this.cache.size >= 512) this.cache.delete(this.cache.keys().next().value);
        this.cache.set(index, page);
      }
    }
    if (write && !this.owned.has(index)) {
      const copy = Buffer.alloc(PAGE); page.copy(copy); page = copy;
      this.pages.set(index, page); this.owned.add(index);
    }
    return page;
  }
  check(at, n) { if (!Number.isSafeInteger(at) || at < 0 || at + n > this.length) throw Error('image access outside bounds'); }
  readUInt8(at) { this.check(at, 1); return this.page(Math.floor(at / PAGE))[at % PAGE]; }
  writeUInt8(value, at) { this.check(at, 1); this.page(Math.floor(at / PAGE), true)[at % PAGE] = value; }
  readUInt16LE(at) {
    this.check(at, 2);
    return at % PAGE < PAGE - 1 ? this.page(Math.floor(at / PAGE)).readUInt16LE(at % PAGE)
                              : this.readUInt8(at) + this.readUInt8(at + 1) * 256;
  }
  readUInt32LE(at) { return this.readUInt16LE(at) + this.readUInt16LE(at + 2) * 65536; }
  writeUInt16LE(value, at) {
    this.check(at, 2);
    if (!Number.isInteger(value) || value < 0 || value > 65535) throw Error('invalid image word');
    if (at % PAGE < PAGE - 1) this.page(Math.floor(at / PAGE), true).writeUInt16LE(value, at % PAGE);
    else this.set(Buffer.from([value & 255, value >> 8]), at);
  }
  writeUInt32LE(value, at) {
    if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) throw Error('invalid image dword');
    this.check(at, 4); this.writeUInt16LE(value % 65536, at); this.writeUInt16LE(Math.floor(value / 65536), at + 2);
  }
  subarray(start = 0, end = this.length) {
    start = Math.min(start, this.length); end = Math.min(end, this.length);
    this.check(start, end - start);
    const result = Buffer.alloc(end - start);
    for (let at = start; at < end;) {
      const n = Math.min(PAGE - at % PAGE, end - at);
      this.page(Math.floor(at / PAGE)).copy(result, at - start, at % PAGE, at % PAGE + n); at += n;
    }
    return result;
  }
  set(data, at = 0) {
    if (Array.isArray(data)) data = Buffer.from(data);
    this.check(at, data.length);
    for (let done = 0; done < data.length;) {
      const n = Math.min(PAGE - at % PAGE, data.length - done);
      const part = data.subarray(done, done + n), index = Math.floor(at / PAGE);
      // New zero-backed images retain holes, including when copying huge roots.
      if (this.source || this.pages.has(index) || !part.equals(ZERO.subarray(0, n)))
        part.copy(this.page(index, true), at % PAGE);
      done += n; at += n;
    }
  }
  copy(target, at = 0, start = 0, end = this.length) {
    this.check(start, end - start);
    // Each range is buffered before writing, so small overlapping metadata moves work.
    if (target === this) { this.set(this.subarray(start, end), at); return; }
    for (let pos = start; pos < end; pos += 65536) {
      const chunk = this.subarray(pos, Math.min(end, pos + 65536));
      if (target instanceof Image) target.set(chunk, at + pos - start);
      else chunk.copy(target, at + pos - start);
    }
  }
  fill(value, start = 0, end = this.length) {
    this.check(start, end - start);
    const chunk = Buffer.alloc(PAGE, value);
    for (let at = start; at < end; at += PAGE) this.set(chunk.subarray(0, Math.min(PAGE, end - at)), at);
    return this;
  }
  write(text, at = 0, encoding = 'utf8') { this.set(Buffer.from(text, encoding), at); }
  toString(encoding, start = 0, end = this.length) { return this.subarray(start, end).toString(encoding); }
  equals(other) {
    other = Image.from(other);
    if (this.length !== other.length) return false;
    if (this.source === other.source) {
      for (const index of new Set([...this.pages.keys(), ...other.pages.keys()]))
        if (!this.page(index).equals(other.page(index))) return false;
      return true;
    }
    for (let at = 0; at < this.length; at += 65536)
      if (!this.subarray(at, Math.min(at + 65536, this.length)).equals(other.subarray(at, Math.min(at + 65536, this.length)))) return false;
    return true;
  }
}
export function openImage(filename) {
  const fd = fs.openSync(filename, 'r'), length = fs.fstatSync(fd).size, cache = new Map();
  const source = {
    filename, fd,
    read(at, n) {
      const index = Math.floor(at / PAGE);
      if (!cache.has(index)) {
        const data = Buffer.alloc(PAGE); fs.readSync(fd, data, 0, Math.min(PAGE, Math.max(0, length - at)), at);
        if (cache.size >= 512) cache.delete(cache.keys().next().value);
        cache.set(index, data);
      }
      return cache.get(index).subarray(0, n);
    }
  };
  return new Image(length, source);
}
export function writeImage(filename, value, exclusive = false) {
  const image = Image.from(value);
  if (image.source?.fd !== undefined) {
    const original = fs.fstatSync(image.source.fd);
    try {
      const target = fs.statSync(filename);
      if (target.ino === original.ino && target.dev === original.dev)
        throw Error('refusing to overwrite a backing image in place');
    } catch (error) { if (error.code !== 'ENOENT') throw error; }
  }
  let clone = false;
  if (image.source?.filename) {
    try {
      const named = fs.statSync(image.source.filename), original = fs.fstatSync(image.source.fd);
      clone = named.ino === original.ino && named.dev === original.dev;
    } catch (error) { if (error.code !== 'ENOENT') throw error; }
  }
  if (clone) {
    try {
      // Optional FICLONE silently falls back to a dense copy on platforms
      // without clone support (including some macOS Node builds). Preserve
      // holes with the bounded sparse writer below instead of filling disks.
      fs.copyFileSync(image.source.filename, filename, fs.constants.COPYFILE_FICLONE_FORCE | (exclusive ? fs.constants.COPYFILE_EXCL : 0));
    } catch (error) {
      if (!['ENOSYS', 'ENOTSUP', 'EOPNOTSUPP', 'EXDEV'].includes(error.code)) throw error;
      clone = false;
    }
  }
  const fd = fs.openSync(filename, clone ? 'r+' : exclusive ? 'wx' : 'w', 0o600);
  try {
    fs.ftruncateSync(fd, image.length);
    if (image.source && !clone) {
      for (let at = 0; at < image.length; at += 65536) {
        const chunk = image.subarray(at, Math.min(at + 65536, image.length));
        if (!chunk.equals(Buffer.alloc(chunk.length))) fs.writeSync(fd, chunk, 0, chunk.length, at);
      }
    } else for (const [index, page] of image.pages)
      if (index * PAGE < image.length) fs.writeSync(fd, page, 0, Math.min(PAGE, image.length - index * PAGE), index * PAGE);
    fs.fsyncSync(fd);
  } finally { fs.closeSync(fd); }
}
