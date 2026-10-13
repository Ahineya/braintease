#!/usr/bin/env node
// File-format work for the Make/Bash build; compilation stays in the Makefile.
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { WAD_MODES, checkMode, packAssets, cachedAssets, hasCurrentAssetValidation, assertSeparateFile, writeAtomic } from './assets-pack.mjs';
import { assetGeneratorHash, assetValidatorHash } from './assets-identity.mjs';

const sha = bytes => createHash('sha256').update(bytes).digest('hex');
function write(file, content) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  if (fs.existsSync(file) && fs.readFileSync(file, 'utf8') === content) return;
  const temporary = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(temporary, content); fs.renameSync(temporary, file);
}
const json = value => `${JSON.stringify(value, null, 2)}\n`;
const port = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
function checkValidation(value) {
  if (!['preflight', 'guest'].includes(value)) throw new Error('asset validation must be preflight or guest');
  return value;
}
function sourceIdentity() {
  const files = ['Makefile', 'run.sh', 'run-bf.sh', 'upstream.json', 'tests/programs.mk'];
  function walk(directory) {
    for (const entry of fs.readdirSync(path.join(port, directory), { withFileTypes: true })) {
      const relative = `${directory}/${entry.name}`;
      if (entry.isDirectory()) walk(relative);
      else if (/\.(?:c|h|asm|mjs|sh|rs|inc)$/.test(entry.name)) files.push(relative);
    }
  }
  for (const directory of ['engine', 'src', 'include', 'tools', 'tests']) walk(directory);
  const hashes = Object.fromEntries(files.sort().map(file => [file, sha(fs.readFileSync(path.join(port, file)))]));
  return { sha256: sha(JSON.stringify(hashes)), files: hashes };
}

try {
  const [action, ...args] = process.argv.slice(2);
  if (action === 'config-key') {
    const [source, mode = 'cooked', bank = '64000', generated = '', assetValidation = 'preflight'] = args;
    checkMode(mode);
    checkValidation(assetValidation);
    const wad = fs.realpathSync(source);
    // Make compares this content key per program, independent of the shared
    // header's mtime. Include the generator so layout changes invalidate old
    // consumers even when the original WAD is unchanged.
    process.stdout.write(`${sha(json({ wad, source: sha(fs.readFileSync(wad)), mode, bank, generated: path.resolve(generated), asset_validation: assetValidation,
      generator: assetGeneratorHash(),
      validator: assetValidatorHash(),
      config: sha(fs.readFileSync(fileURLToPath(import.meta.url))) }))}\n`);
  } else if (action === 'config-stamp') {
    const [file, key] = args;
    if (!/^[0-9a-f]{64}$/.test(key || '')) throw new Error('invalid build configuration key');
    write(file, `${key}\n`);
  } else if (action === 'config-invalidate') {
    for (const file of args) fs.rmSync(file, { force: true });
  } else if (action === 'config') {
    const [source, generated, manifest, bank = '64000', program = 'engine', startMap = '0', lowDetail = '0', defines = '', rippleFlags = '', cc = 'clang', cflags = '', cppflags = '', ldflags = '', wadMode = 'cooked', assetValidation = 'preflight'] = args;
    const wad = fs.realpathSync(source), bytes = fs.readFileSync(wad);
    checkMode(wadMode);
    checkValidation(assetValidation);
    const asset = cachedAssets(wad, path.join(path.dirname(manifest), 'assets', `doom-${wadMode}.img`), wadMode);
    const validation = asset.validation ?? null;
    write(path.join(generated, 'doom_build_config.h'), `/* Generated: WAD lengths count octets; asset offsets count 16-bit disk words. */\n#ifndef DOOM_BUILD_CONFIG_H\n#define DOOM_BUILD_CONFIG_H\n#define DOOM_WAD_LENGTH ${bytes.length}UL\n#define DOOM_WAD_PATH ${JSON.stringify(wad)}\n#define DOOM_WAD_SHA256 ${JSON.stringify(sha(bytes))}\n#define DOOM_WAD_MODE ${WAD_MODES[wadMode]}\n#define DOOM_ASSET_PATH ${JSON.stringify(asset.path)}\n#define DOOM_ASSET_VERSION ${asset.format_version}U\n#define DOOM_ASSET_HEADER_WORD ${asset.header_word}UL\n#define DOOM_ASSET_INDEX_WORD ${asset.index_word}UL\n#define DOOM_ASSET_DIRECTORY_WORD ${asset.directory_word}UL\n#define DOOM_ASSET_HASH_HEADS_WORD ${asset.hash_heads_word}UL\n#define DOOM_ASSET_PNAMES_WORD ${asset.pnames_word}UL\n#define DOOM_ASSET_PNAMES_COUNT ${asset.pnames_count}UL\n#define DOOM_ASSET_PNAMES_LUMP ${asset.pnames_lump}UL\n#define DOOM_ASSET_TEXTURE_WORD ${asset.texture_word}UL\n#define DOOM_ASSET_TEXTURE_WORDS ${asset.texture_words}UL\n#define DOOM_ASSET_TEXTURE_COUNT ${asset.texture_count}UL\n#define DOOM_ASSET_TEXTURE_COLUMNS ${asset.texture_columns}UL\n#define DOOM_ASSET_TEXTURE_DEFINITION_WORDS ${asset.texture_definition_words}UL\n#define DOOM_ASSET_SPRITE_WORD ${asset.sprite_word}UL\n#define DOOM_ASSET_SPRITE_WORDS ${asset.sprite_words}UL\n#define DOOM_ASSET_SPRITE_COUNT ${asset.sprite_count}UL\n#define DOOM_ASSET_SPRITE_FRAMES ${asset.sprite_frames}UL\n#define DOOM_ASSET_SPRITE_FIRST_LUMP ${asset.sprite_first_lump}UL\n#define DOOM_ASSET_SPRITE_LAST_LUMP ${asset.sprite_last_lump}UL\n#define DOOM_ASSET_SPRITE_NAMES_LO ${asset.sprite_names_lo}UL\n#define DOOM_ASSET_SPRITE_NAMES_HI ${asset.sprite_names_hi}UL\n#define DOOM_ASSET_RENDERER_WORD ${asset.renderer_word}UL\n#define DOOM_ASSET_RENDERER_WORDS ${asset.renderer_words}UL\n#define DOOM_ASSET_SPRITE_METRICS_WORD ${asset.sprite_metrics_word}UL\n#define DOOM_ASSET_SPRITE_METRICS_WORDS ${asset.sprite_metrics_words}UL\n#define DOOM_ASSET_METADATA_ABI ${asset.metadata_abi}U\n#define DOOM_ASSET_DIRECTORY_RECORD_WORDS ${asset.directory_record_words}U\n#define DOOM_ASSET_HASH_ENTRY_WORDS ${asset.hash_entry_words}U\n#define DOOM_ASSET_PNAMES_ENTRY_WORDS ${asset.pnames_entry_words}U\n#define DOOM_ASSET_TOTAL_WORDS ${asset.total_words}UL\n#define DOOM_ASSET_LUMP_COUNT ${asset.lump_count}UL\n#define DOOM_ASSET_FINGERPRINT_LO ${asset.fingerprint_lo}U\n#define DOOM_ASSET_FINGERPRINT_HI ${asset.fingerprint_hi}U\n#endif\n`);
    const upstream = JSON.parse(fs.readFileSync(path.join(port, 'upstream.json')));
    write(manifest, json({ source_model: 'maintained engine/ sources', upstream: { project: upstream.project, repository: upstream.repository, revision: upstream.revision },
      sources: sourceIdentity(), settings: { program, start_map: startMap === '1', low_detail: lowDetail === '1', wad_mode: wadMode, asset_validation: assetValidation, defines, ripple_flags: rippleFlags, cc, cflags, cppflags, ldflags },
      wad: { path: wad, octets: bytes.length, sha256: sha(bytes) }, asset_image: asset, asset_preflight: validation, bank_size: Number(bank), memory_cells: 256 * Number(bank) }));
  } else if (action === 'stamp') {
    write(args[0], json(args.slice(1)));
  } else if (action === 'disk') {
    // Two arguments remain packed for existing standalone fixtures. Launchers
    // pass mode and build manifest so disk bytes match the compiled program.
    const [source, destination, mode = 'packed', manifest] = args;
    checkMode(mode);
    const wad = fs.realpathSync(source);
    const bytes = fs.readFileSync(wad);
    let image, metadata, disk;
    if (manifest) {
      const build = JSON.parse(fs.readFileSync(manifest, 'utf8'));
      metadata = build.asset_image;
      if (!metadata || build.settings?.wad_mode !== mode || metadata.mode !== mode
          || build.wad?.sha256 !== sha(bytes) || metadata.source_sha256 !== sha(bytes))
        throw new Error('build manifest WAD or asset mode does not match the requested disk');
      disk = assertSeparateFile(destination, [wad, metadata.path]);
      image = fs.readFileSync(metadata.path);
      if (sha(image) !== metadata.sha256 || image.length !== metadata.octets)
        throw new Error('cached asset image differs from build manifest; rebuild before running');
      if (mode === 'cooked' && !hasCurrentAssetValidation(metadata))
        throw new Error('cooked asset image has no current build validation; rebuild before running');
    } else {
      disk = assertSeparateFile(destination, [wad]);
      ({ image, metadata } = packAssets(bytes, mode));
    }
    // The packer already checked the complete serialized artifact. Staging
    // only pairs those exact bytes with the build; no Doom semantics run here.
    const validation = metadata.validation ?? null;
    writeAtomic(disk, image);
    process.stdout.write(json({ ...metadata, path: disk, asset_preflight: validation }));
  } else if (action === 'bf-tape-cells') {
    // Only read the small layout header; expanded Doom BF can be very large.
    const fd = fs.openSync(args[0], 'r');
    const header = Buffer.alloc(512);
    let length;
    try { length = fs.readSync(fd, header, 0, header.length, 0); }
    finally { fs.closeSync(fd); }
    const match = /^BFHOST 1 BANK_SIZE \d+ MEMORY_BANKS \d+ INSTRUCTIONS \d+ ENTRY \d+ ORIGIN 8 TAPE_CELLS ([1-9]\d*) END_BFHOST\r?\n/.exec(header.toString('ascii', 0, length));
    if (!match || !Number.isSafeInteger(Number(match[1])))
      throw new Error('missing or invalid BFHOST tape size in the expanded image');
    process.stdout.write(`${match[1]}\n`);
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
  } else throw new Error('expected config, config-key, config-stamp, config-invalidate, stamp, disk, bf-tape-cells, run-manifest or native-flags');
} catch (error) { console.error(`error: ${error.message}`); process.exitCode = 1; }
