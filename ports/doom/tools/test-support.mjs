// Shared test/benchmark orchestration. Production builds do not depend on this.
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

export const PORT = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
export const ROOT = path.dirname(path.dirname(PORT));
export const BUILD = process.env.DOOM_BUILD ? path.resolve(process.env.DOOM_BUILD) : path.join(PORT, 'build');
export const WAD = path.join(PORT, 'assets/doom1.wad');
export const RVM = path.join(ROOT, 'target/release/rvm');
export const RLIB = path.join(ROOT, 'target/release/librvm.rlib');
export const BANK = 64000;
export const MEMORY = 16384000;
export const commands = [];
export const hash = data => createHash('sha256').update(data).digest('hex');
export const digest = file => hash(fs.readFileSync(file));
export const json = file => JSON.parse(fs.readFileSync(file, 'utf8'));
export const mkdir = directory => fs.mkdirSync(directory, { recursive: true });
export function require(condition, message) { if (!condition) throw new Error(message); }
export function writeJSON(file, value) {
  mkdir(path.dirname(file));
  fs.writeFileSync(file, JSON.stringify(value, null, 2) + '\n');
}
export function run(label, command, out, timeout = 120) {
  mkdir(out);
  const stdout = path.join(out, label + '.stdout.log');
  const stderr = path.join(out, label + '.stderr.log');
  const record = { label, command: command.map(String), stdout, stderr };
  commands.push(record);
  const handles = [fs.openSync(stdout, 'w'), fs.openSync(stderr, 'w')];
  const start = performance.now();
  let result;
  try {
    result = spawnSync(record.command[0], record.command.slice(1), {
      cwd: ROOT, stdio: ['ignore', ...handles], timeout: timeout * 1000,
    });
  } finally {
    handles.forEach(fd => fs.closeSync(fd));
    record.seconds = (performance.now() - start) / 1000;
  }
  record.exit_code = result.status;
  if (result.error || result.status !== 0) {
    record.error = result.error?.message || 'exit ' + result.status;
    const tail = fs.readFileSync(stderr, 'utf8').slice(-3000);
    throw new Error(label + ' failed (' + record.error + '); see ' + stdout + ' and ' + stderr + '\n' + tail);
  }
  return fs.readFileSync(stdout, 'utf8');
}
export const WAD_MODES = ['packed', 'expanded', 'cooked'];
export function prepareDisk(wad, disk, { wadMode = 'packed', manifest } = {}) {
  require(WAD_MODES.includes(wadMode), 'unknown WAD storage mode: ' + wadMode);
  const source = fs.realpathSync(wad);
  const destination = fs.existsSync(disk) ? fs.realpathSync(disk) : path.resolve(disk);
  require(source !== destination, 'VM disk must differ from source WAD');
  if (fs.existsSync(disk)) {
    const a = fs.statSync(source), b = fs.statSync(disk);
    require(a.dev !== b.dev || a.ino !== b.ino, 'VM disk must not be a hard link to source WAD');
  }
  const data = fs.readFileSync(source);
  if (manifest) {
    const built = json(manifest), asset = built.asset_image;
    require(built.wad?.sha256 === hash(data) && built.settings?.wad_mode === wadMode,
      'build manifest does not match requested WAD or storage mode; rebuild before testing');
    require(asset && asset.mode === wadMode && asset.source_sha256 === hash(data),
      'build manifest has no matching prebuilt asset image');
    const assetPath = fs.realpathSync(asset.path), image = fs.readFileSync(assetPath);
    require(image.length === asset.octets && hash(image) === asset.sha256,
      'prebuilt asset image differs from build manifest');
    require(destination !== assetPath, 'test disk must differ from prebuilt asset image');
    if (fs.existsSync(disk)) {
      const a = fs.statSync(assetPath), b = fs.statSync(disk);
      require(a.dev !== b.dev || a.ino !== b.ino, 'test disk must not be a hard link to asset image');
    }
    mkdir(path.dirname(disk)); fs.writeFileSync(disk, image);
    return { ...asset, path: disk, prepared_path: assetPath, build_manifest: path.resolve(manifest) };
  }
  require(wadMode === 'packed', 'expanded/cooked tests require a matching build manifest');
  const image = Buffer.alloc(Math.ceil(data.length / 65536) * 65536);
  data.copy(image);
  mkdir(path.dirname(disk));
  fs.writeFileSync(disk, image);
  return { path: disk, mode: wadMode, octets: image.length, padding_octets: image.length - data.length, sha256: hash(image) };
}
export function build(program, out, { wad = WAD, wadMode = process.env.DOOM_WAD_MODE || 'cooked', target = 'both', startMap = false, lowDetail = false } = {}) {
  require(WAD_MODES.includes(wadMode), 'unknown WAD storage mode: ' + wadMode);
  console.log('Building ' + program + ' (' + target + ')');
  run('build-' + program + (startMap ? '-map' : '-title'), ['make', '-C', PORT,
    'PROGRAM=' + program, 'TARGET=' + target, 'WAD=' + path.resolve(wad), 'WAD_MODE=' + wadMode, 'BUILD=' + BUILD,
    'START_MAP=' + Number(startMap), 'LOW_DETAIL=' + Number(lowDetail), 'all'], out, 900);
}
export function compileRust(source, output, out) {
  run('build-' + path.basename(output), ['rustc', '--edition=2021', '-O', '-C', 'lto=thin', source,
    '--extern', 'rvm=' + RLIB, '-L', 'dependency=' + path.join(ROOT, 'target/release/deps'), '-o', output], out, 180);
}
export function frameRows(output) {
  return [...output.matchAll(/^ENGINE frame (\d+) tick (\d+) state (\d+) episode (\d+) map (\d+)$/gm)]
    .map(match => match.slice(1).map(Number));
}
export function nativeInput(script, output) {
  const events = json(script);
  require(Array.isArray(events) && events.every(event => Number.isSafeInteger(event.key)
    && event.key >= 0 && event.key < 256 && Number.isSafeInteger(event.tick)
    && event.tick >= 0 && typeof event.down === 'boolean'), 'native replay needs numeric HID keys and valid ticks');
  fs.writeFileSync(output, events.map(e => [e.tick, e.key, Number(e.down)].join(' ') + '\n').join(''));
}
