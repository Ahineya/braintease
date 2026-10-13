// SPDX-License-Identifier: GPL-2.0-or-later
// Build immutable renderer tables for every supported view size and detail.
// Values are 16-bit Ripple C cells; offsets are relative to this section.
import fs from 'node:fs';

export const RENDERER_HEADER_WORDS = 16;
export const RENDERER_RECORD_WORDS = 16;
export const RENDERER_VIEW_COUNT = 18;

function source(name) { return fs.readFileSync(new URL('../engine/' + name, import.meta.url), 'utf8'); }
function constant(text, name, expected) {
  const match = text.match(new RegExp('^#define\\s+' + name + '\\s+(\\d+)\\s*$', 'm'));
  if (!match || Number(match[1]) !== expected) throw new Error('Renderer constant changed: ' + name);
}
export function rendererSourceTables() {
  const main = source('r_main.h'), tables = source('tables.h'), fixed = source('m_fixed.h');
  const renderer = source('r_main.c');
  const video = source('i_video.h');
  for (const [name, value] of Object.entries({ LIGHTLEVELS: 16, MAXLIGHTSCALE: 48,
    LIGHTSCALESHIFT: 12, MAXLIGHTZ: 128, LIGHTZSHIFT: 20, NUMCOLORMAPS: 32 })) constant(main, name, value);
  constant(tables, 'FINEANGLES', 8192); constant(tables, 'ANGLETOFINESHIFT', 19);
  constant(fixed, 'FRACBITS', 16);
  constant(renderer, 'FIELDOFVIEW', 2048); constant(renderer, 'DISTMAP', 2);
  constant(video, 'SCREENWIDTH', 320); constant(video, 'SCREENHEIGHT', 200);
  const text = source('tables.c');
  function read(name, count) {
    const match = text.match(new RegExp('const\\s+\\w+\\s+' + name + '\\s*\\[\\s*' + count + '\\s*\\]\\s*=\\s*\\{([\\s\\S]*?)\\}\\s*;'));
    if (!match) throw new Error('Missing renderer source table: ' + name);
    const values = match[1].replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, '').split(',').map(v => v.trim()).filter(Boolean).map(v => {
      const literal = v.replace(/[uUlL]+$/, '');
      if (!/^-?(?:0x[\da-f]+|\d+)$/i.test(literal)) throw new Error('Invalid renderer literal: ' + v);
      return Number(literal);
    });
    if (values.length !== count || values.some(v => !Number.isInteger(v) || v < -2147483648 || v > 2147483647))
      throw new Error('Invalid renderer source table: ' + name);
    return values;
  }
  return { tangent: read('finetangent', 4096), sine: read('finesine', 10240) };
}

export function rendererFixedMul(a, b) {
  return Number(BigInt.asIntN(32, (BigInt(a) * BigInt(b)) >> 16n));
}
export function rendererFixedDiv(a, b) {
  if (Math.floor(Math.abs(a) / 16384) >= Math.abs(b))
    return (a < 0) !== (b < 0) ? -2147483648 : 2147483647;
  return Number(BigInt(a) * 65536n / BigInt(b));
}
const clampLight = n => Math.max(0, Math.min(31, n));

export function packRendererTables() {
  const { tangent, sine } = rendererSourceTables();
  const words = Array(RENDERER_HEADER_WORDS + RENDERER_VIEW_COUNT * RENDERER_RECORD_WORDS).fill(0);
  function append(values, wide = false) {
    const at = words.length;
    for (const value of values) {
      words.push(value & 65535);
      if (wide) words.push((value >>> 16) & 65535);
    }
    return at;
  }
  const zlight = [];
  for (let light = 0; light < 16; light++) for (let distance = 0; distance < 128; distance++) {
    const scale = rendererFixedDiv(160 * 65536, (distance + 1) * 1048576) >> 12;
    zlight.push(clampLight((15 - light) * 4 - Math.trunc(scale / 2)));
  }
  const zoffset = append(zlight);
  words.splice(0, 7, 0x5256, 0x5442, 1, RENDERER_VIEW_COUNT, zoffset & 65535, zoffset >>> 16, 2048);
  let record = RENDERER_HEADER_WORDS;
  for (let blocks = 3; blocks <= 11; blocks++) for (let detail = 0; detail <= 1; detail++) {
    const scaledwidth = blocks === 11 ? 320 : blocks * 32;
    const height = blocks === 11 ? 200 : Math.trunc(blocks * 168 / 10) & ~7;
    const width = scaledwidth >> detail, center = Math.trunc(width / 2) * 65536;
    const focal = rendererFixedDiv(center, tangent[3072]);
    const angletox = tangent.map(value => value > 131072 ? -1 : value < -131072 ? width + 1
      : Math.max(-1, Math.min(width + 1, (center - rendererFixedMul(value, focal) + 65535) >> 16)));
    const xtoangle = Array(width + 1);
    let cursor = 0;
    for (let x = width; x >= 0; x--) {
      while (angletox[cursor] > x) cursor++;
      xtoangle[x] = (cursor * 524288 - 0x40000000) >>> 0;
    }
    const yslopes = Array.from({ length: height }, (_, y) => rendererFixedDiv(scaledwidth / 2 * 65536,
      Math.abs((y - Math.trunc(height / 2)) * 65536 + 32768)));
    const distances = xtoangle.slice(0, width).map(angle => rendererFixedDiv(65536,
      Math.abs(sine[2048 + (angle >>> 19)])));
    const lights = [];
    for (let light = 0; light < 16; light++) for (let scale = 0; scale < 48; scale++)
      lights.push(clampLight((15 - light) * 4 - Math.trunc(Math.trunc(scale * 320 / scaledwidth) / 2)));
    const offsets = [append(angletox.map(x => Math.max(0, Math.min(width, x)))),
      append(xtoangle, true), append(yslopes, true), append(distances, true), append(lights)];
    const fields = [blocks, detail, scaledwidth, height, width, 0,
      ...offsets.flatMap(offset => [offset & 65535, offset >>> 16])];
    words.splice(record, RENDERER_RECORD_WORDS, ...fields);
    record += RENDERER_RECORD_WORDS;
  }
  const buffer = Buffer.alloc(words.length * 2);
  words.forEach((word, i) => buffer.writeUInt16LE(word, i * 2));
  return { buffer, words: words.length, views: RENDERER_VIEW_COUNT };
}
