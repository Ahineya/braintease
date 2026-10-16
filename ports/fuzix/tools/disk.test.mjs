import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import test from 'node:test';

import {buildDisk, rootImage} from './disk.mjs';
import {fatFile} from './fat.mjs';
import {checkRoot} from './fsck.mjs';
import {installRoot, growRoot} from './install.mjs';

const seed = [ {name : '/init', data : Buffer.alloc(75000, 0x61)} ];
test('launcher recovers dirty metadata with a backup, preserves no-op identity, and refuses structural damage', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fuzix-launch-recovery-'));
  try {
    for (const name of ['init', 'hello', 'forkcheck', 'preemptcheck', 'iotest'])
      fs.writeFileSync(path.join(dir, `${name}.rxe`), Buffer.from(name));
    fs.writeFileSync(path.join(dir, 'FUZIX.SYS'), 'kernel');
    const disk = path.join(dir, 'disk.img');
    buildDisk(dir, disk);
    const original = fs.readFileSync(disk), root = fatFile(original, 'ROOTFS.IMG');
    const withNote = installRoot(root.data, [{name: '/note', mode: 0o100640,
                                            data: Buffer.from('saved session')}]);
    withNote.writeUInt16LE(1, 361 * 2);
    const dirty = root.replace(withNote);
    fs.writeFileSync(disk, dirty);
    const launcher = fileURLToPath(new URL('./launch.py', import.meta.url));
    const launch = () => spawnSync('python3', [launcher, '--build', dir, '--disk', disk,
                                             '--vm', '/unused', '--update', '--build-only'],
                                  {encoding: 'utf8'});
    const recovered = launch();
    assert.equal(recovered.status, 0, recovered.stderr);
    assert.match(recovered.stdout, /Recovered the filesystem/);
    assert.deepEqual(fs.readFileSync(`${disk}.bak`), dirty);
    const good = fs.readFileSync(disk), cleanRoot = fatFile(good, 'ROOTFS.IMG').data;
    assert.deepEqual(checkRoot(cleanRoot).issues, []);
    // User inodes, contents, and permissions survive the recovery and update.
    const note = Buffer.from('saved session', 'utf16le');
    assert.ok(cleanRoot.includes(note));
    const before = fs.statSync(disk).mtimeMs;
    assert.equal(launch().status, 0);
    assert.equal(fs.statSync(disk).mtimeMs, before);
    assert.deepEqual(fs.readFileSync(disk), good);
    const backups = fs.readdirSync(dir).filter(n => n.startsWith('disk.img.bak'));

    // Reuse init's data block in hello: this is ambiguous corruption, not a
    // dirty shutdown. Automatic recovery must leave the entire image alone.
    const brokenRoot = Buffer.from(cleanRoot);
    const base = ino => (2 + (ino >> 3)) * 256 + (ino & 7) * 32;
    brokenRoot.writeUInt16LE(brokenRoot.readUInt16LE((base(7) + 12) * 2),
                            (base(8) + 12) * 2);
    const broken = fatFile(good, 'ROOTFS.IMG').replace(brokenRoot);
    fs.writeFileSync(disk, broken);
    const refused = launch();
    assert.equal(refused.status, 1);
    assert.match(refused.stderr, /multiply allocated/);
    assert.doesNotMatch(refused.stderr, /Traceback|file:\/\/\//);
    assert.deepEqual(fs.readFileSync(disk), broken);
    assert.deepEqual(fs.readdirSync(dir).filter(n => n.startsWith('disk.img.bak')), backups);
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});
test('checker and upgrades retain allocated named-pipe buffers', () => {
  const root = rootImage(
      [{name: '/fifo', mode: 0o10600, data: Buffer.from('pipe buffer')}],
      8192, 128);
  assert.deepEqual(checkRoot(root).issues, []);
  const grown = growRoot(root);
  const installed = installRoot(grown, [{name: '/bin/tool', data: Buffer.from('new')}]);
  // The first seeded file is inode 7. Its early buffer must be relocated
  // when the larger inode table is installed, then kept out of free lists.
  const base = (2 + (7 >> 3)) * 256 + (7 & 7) * 32;
  const block = installed.readUInt16LE((base + 12) * 2);
  assert.ok(block >= 8192);
  assert.deepEqual(checkRoot(installed).issues, []);
  assert.deepEqual(Array.from({length: 11}, (_, i) =>
      installed.readUInt16LE((block * 256 + i) * 2)),
      Array.from(Buffer.from('pipe buffer')));
});
test('C16 root has valid double-indirect files, directory links and free lists',
     () => {
       const image = rootImage(seed);
       assert.equal(checkRoot(image).issues.length, 0);
       assert.deepEqual(image, rootImage(seed));
     });
test(
    'checker repairs dirty/free-list metadata but rejects ambiguous structural corruption',
    () => {
      const image = rootImage(seed);
      image.writeUInt16LE(1, (256 + 105) * 2);
      image.writeUInt16LE(999, (256 + 109) * 2);
      assert.equal(checkRoot(image).issues.length, 2);
      const fixed = checkRoot(image, true).image;
      assert.deepEqual(checkRoot(fixed).issues, []);
      const broken = Buffer.from(image);
      broken.writeUInt16LE(2, (2 * 256 + 32 + 12) * 2);
      assert.throws(() => checkRoot(broken, true), /outside data area/);
    });
test(
    'kernel updates preserve every root byte and no-op updates preserve disk timestamps',
    () => {
      const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fuzix-disk-'));
      try {
        for (const name
                 of ['init', 'hello', 'forkcheck', 'preemptcheck', 'iotest'])
          fs.writeFileSync(path.join(dir, `${name}.rxe`), Buffer.from(name));
        fs.writeFileSync(path.join(dir, 'FUZIX.SYS'), 'first kernel');
        const disk = path.join(dir, 'disk.img');
        buildDisk(dir, disk);
        let image = fs.readFileSync(disk), root = fatFile(image, 'ROOTFS.IMG');
        root.data[450] = 0x5a;
        image = root.replace(root.data);
        fs.writeFileSync(disk, image);
        const expected = fatFile(image, 'ROOTFS.IMG').data;
        fs.writeFileSync(path.join(dir, 'FUZIX.SYS'),
                         Buffer.alloc(24000, 0x5e));
        assert.equal(buildDisk(dir, disk), true);
        assert.deepEqual(fatFile(fs.readFileSync(disk), 'ROOTFS.IMG').data,
                         expected);
        assert.deepEqual(fs.readFileSync(`${disk}.bak`), image);
        const before = fs.statSync(disk).mtimeMs;
        assert.equal(buildDisk(dir, disk), false);
        assert.equal(fs.statSync(disk).mtimeMs, before);
      } finally {
        fs.rmSync(dir, {recursive : true, force : true});
      }
    });
test(
    'FAT mapping refuses truncated or circular chains and replacement resizing',
    () => {
      const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fuzix-fat-'));
      try {
        for (const name
                 of ['init', 'hello', 'forkcheck', 'preemptcheck', 'iotest'])
          fs.writeFileSync(path.join(dir, `${name}.rxe`), 'seed');
        fs.writeFileSync(path.join(dir, 'FUZIX.SYS'), 'kernel');
        const disk = path.join(dir, 'disk.img');
        buildDisk(dir, disk);
        const image = fs.readFileSync(disk);
        assert.throws(() =>
                          fatFile(image, 'ROOTFS.IMG').replace(Buffer.alloc(1)),
                      /resize/);
        assert.throws(() => fatFile(image.subarray(0, 1000), 'ROOTFS.IMG'));
        const cyclic = Buffer.from(image),
              root = (cyclic.readUInt16LE(14) +
                      cyclic[16] * cyclic.readUInt16LE(22)) *
                     cyclic.readUInt16LE(11),
              first = cyclic.readUInt16LE(root + 32 + 26);
        for (let n = 0; n < 2; n++)
          cyclic.writeUInt16LE(first, 512 + n * cyclic.readUInt16LE(22) * 512 +
                                          first * 2);
        assert.throws(() => fatFile(cyclic, 'ROOTFS.IMG'), /chain/);
      } finally {
        fs.rmSync(dir, {recursive : true, force : true});
      }
    });

test(
    'userspace upgrades preserve user data, hard links, configuration, and no-op identity',
    async () => {
      const {installRoot, growRoot} = await import('./install.mjs');
      let root = rootImage(
          [
            {name : '/init', data : Buffer.from('old init')},
            {name : '/note', mode : 0o100640, data : Buffer.from('keep me')}, {
              name : '/etc/passwd',
              mode : 0o100644,
              data : Buffer.from('custom passwd\n')
            }
          ],
          8192, 128);
      const before = Buffer.from(root);
      const files = [
        {name : '/init', data : Buffer.from('new init')},
        ...Array.from(
            {length : 40},
            (_, i) =>
                ({name : `/bin/tool${i}`, data : Buffer.alloc(800 + i, i)})),
        {
          name : '/etc/passwd',
          mode : 0o100644,
          preserve : true,
          data : Buffer.from('default passwd\n')
        }
      ];
      function file(image, name) {
        const w = p => image.readUInt16LE(p * 2);
        let ino = 1;
        for (const part of name.split('/').filter(Boolean)) {
          const base = (2 + (ino >> 3)) * 256 + (ino & 7) * 32,
                size = w(base + 4) + w(base + 5) * 65536;
          let found = 0;
          for (let off = 0; off < size; off += 32) {
            const b = w(base + 12 + Math.floor(off / 256)),
                  p = b * 256 + off % 256;
            let n = '';
            for (let j = 1; j <= 30 && w(p + j); j++)
              n += String.fromCharCode(w(p + j));
            if (n === part) {
              found = w(p);
              break;
            }
          }
          assert.ok(found, `missing ${name}`);
          ino = found;
        }
        const base = (2 + (ino >> 3)) * 256 + (ino & 7) * 32,
              size = w(base + 4) + w(base + 5) * 65536;
        return {
          base,
          mode : w(base),
          data : Array.from(
              {length : size},
              (_, i) => w(w(base + 12 + Math.floor(i / 256)) * 256 + i % 256))
        };
      }
      const note = file(root, '/note'), config = file(root, '/etc/passwd');
      root = installRoot(growRoot(root), files);
      assert.deepEqual(checkRoot(root).issues, []);
      assert.equal(root.length, 65535 * 512);
      assert.deepEqual(file(root, '/note'), note);
      assert.deepEqual(file(root, '/etc/passwd'), config);
      assert.deepEqual(file(root, '/init').data,
                       Array.from(Buffer.from('new init')));
      assert.deepEqual(installRoot(root, files), root);
      assert.deepEqual(before,
                       rootImage(
                           [
                             {name : '/init', data : Buffer.from('old init')}, {
                               name : '/note',
                               mode : 0o100640,
                               data : Buffer.from('keep me')
                             },
                             {
                               name : '/etc/passwd',
                               mode : 0o100644,
                               data : Buffer.from('custom passwd\n')
                             }
                           ],
                           8192, 128));
      const dirty = Buffer.from(root);
      dirty.writeUInt16LE(1, 361 * 2);
      assert.throws(
          () => installRoot(dirty,
                            [ {name : '/bin/new', data : Buffer.from('new')} ]),
          /clean root/);
      assert.throws(() => installRoot(
                        root, [ {name : '/bin', data : Buffer.from('bad')} ]),
                    /nonregular/);
    });

test('replacing a managed executable preserves hard-linked user copies', () => {
  let image = rootImage([
    {name: '/init', data: Buffer.from('old')},
    {name: '/saved', data: Buffer.from('old')}
  ], 8192, 128);
  const w = (b, p) => b.readUInt16LE(p * 2);
  const base = ino => (2 + (ino >> 3)) * 256 + (ino & 7) * 32;
  function entry(b, name) {
    const root = base(1);
    for (let off = 0; off < w(b, root + 4); off += 32) {
      const p = w(b, root + 12 + Math.floor(off / 256)) * 256 + off % 256;
      let text = '';
      for (let j = 1; j <= 30 && w(b, p + j); j++) text += String.fromCharCode(w(b, p + j));
      if (text === name) return p;
    }
    throw Error(`missing ${name}`);
  }
  const init = w(image, entry(image, 'init')), saved = w(image, entry(image, 'saved'));
  image.writeUInt16LE(init, entry(image, 'saved') * 2);
  image.writeUInt16LE(2, (base(init) + 1) * 2);
  image.fill(0, base(saved) * 2, (base(saved) + 32) * 2);
  image = checkRoot(image, true).image;
  const upgraded = installRoot(growRoot(image), [{name: '/init', data: Buffer.from('new')}]);
  for (const [name, expected] of [['init', 'new'], ['saved', 'old']]) {
    const inode = base(w(upgraded, entry(upgraded, name))), block = w(upgraded, inode + 12);
    const data = Array.from({length: w(upgraded, inode + 4)}, (_, i) => w(upgraded, block * 256 + i));
    assert.equal(String.fromCharCode(...data), expected);
    assert.equal(w(upgraded, inode + 1), 1);
  }
  assert.deepEqual(checkRoot(upgraded).issues, []);
});
