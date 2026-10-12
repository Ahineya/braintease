#!/usr/bin/env node
// File-format work for the Make/Bash build; compilation stays in the Makefile.
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const sha = bytes => createHash('sha256').update(bytes).digest('hex');
function write(file, content) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  if (fs.existsSync(file) && fs.readFileSync(file, 'utf8') === content) return;
  const temporary = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(temporary, content); fs.renameSync(temporary, file);
}
const json = value => `${JSON.stringify(value, null, 2)}\n`;
const port = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
function sourceIdentity() {
  const files = ['Makefile', 'run.sh', 'upstream.json', 'tests/programs.mk'];
  function walk(directory) {
    for (const entry of fs.readdirSync(path.join(port, directory), { withFileTypes: true })) {
      const relative = `${directory}/${entry.name}`;
      if (entry.isDirectory()) walk(relative);
      else if (/\.(?:c|h|asm|mjs|sh|rs)$/.test(entry.name)) files.push(relative);
    }
  }
  for (const directory of ['engine', 'src', 'include', 'tools', 'tests']) walk(directory);
  const hashes = Object.fromEntries(files.sort().map(file => [file, sha(fs.readFileSync(path.join(port, file)))]));
  return { sha256: sha(JSON.stringify(hashes)), files: hashes };
}

try {
  const [action, ...args] = process.argv.slice(2);
  if (action === 'config') {
    const [source, generated, manifest, bank = '64000', program = 'engine', startMap = '0', lowDetail = '0', defines = '', rippleFlags = '', cc = 'clang', cflags = '', cppflags = '', ldflags = ''] = args;
    const wad = fs.realpathSync(source), bytes = fs.readFileSync(wad);
    write(path.join(generated, 'doom_build_config.h'), `/* Generated: lengths are serialized octets, not sizeof units. */\n#ifndef DOOM_BUILD_CONFIG_H\n#define DOOM_BUILD_CONFIG_H\n#define DOOM_WAD_LENGTH ${bytes.length}UL\n#define DOOM_WAD_PATH ${JSON.stringify(wad)}\n#define DOOM_WAD_SHA256 ${JSON.stringify(sha(bytes))}\n#endif\n`);
    const upstream = JSON.parse(fs.readFileSync(path.join(port, 'upstream.json')));
    write(manifest, json({ source_model: 'maintained engine/ sources', upstream: { project: upstream.project, repository: upstream.repository, revision: upstream.revision },
      sources: sourceIdentity(), settings: { program, start_map: startMap === '1', low_detail: lowDetail === '1', defines, ripple_flags: rippleFlags, cc, cflags, cppflags, ldflags },
      wad: { path: wad, octets: bytes.length, sha256: sha(bytes) }, bank_size: Number(bank), memory_cells: 256 * Number(bank) }));
  } else if (action === 'stamp') {
    write(args[0], json(args.slice(1)));
  } else if (action === 'disk') {
    const [source, destination] = args;
    const wad = fs.realpathSync(source);
    const disk = fs.existsSync(destination) ? fs.realpathSync(destination) : path.resolve(destination);
    if (wad === disk) throw new Error('the VM disk image must differ from the source WAD');
    if (fs.existsSync(disk)) {
      const sourceStat = fs.statSync(wad), diskStat = fs.statSync(disk);
      if (sourceStat.dev === diskStat.dev && sourceStat.ino === diskStat.ino)
        throw new Error('the VM disk image must not be a hard link to the source WAD');
    }
    const bytes = fs.readFileSync(wad);
    const image = Buffer.alloc(Math.ceil(bytes.length / 65536) * 65536);
    bytes.copy(image);
    fs.mkdirSync(path.dirname(disk), { recursive: true }); fs.writeFileSync(disk, image);
    process.stdout.write(json({ path: disk, octets: image.length, padding_octets: image.length - bytes.length, sha256: sha(image) }));
  } else if (action === 'run-manifest') {
    const [build, disk, output, scene, detail, ...command] = args;
    write(output, json({ ...JSON.parse(fs.readFileSync(build)), disk_image: JSON.parse(fs.readFileSync(disk)), scene, detail, command }));
  } else if (action === 'native-flags') {
    if (process.platform === 'darwin') {
      const cc = args[0] || 'clang';
      let directory = execFileSync(cc, ['-print-resource-dir'], { encoding: 'utf8' }).trim();
      while (directory !== path.dirname(directory)) {
        const name = path.basename(directory);
        const sdk = name === 'Developer' ? path.join(directory, 'Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk') : name === 'CommandLineTools' ? path.join(directory, 'SDKs/MacOSX.sdk') : null;
        if (sdk && fs.existsSync(sdk)) { process.stdout.write(`-isysroot ${JSON.stringify(sdk)}`); break; }
        directory = path.dirname(directory);
      }
    }
  } else throw new Error('expected config, stamp, disk, run-manifest or native-flags');
} catch (error) { console.error(`error: ${error.message}`); process.exitCode = 1; }
