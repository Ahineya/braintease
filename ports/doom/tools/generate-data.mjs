#!/usr/bin/env node
// Generate bounded heap initializers from the directly maintained Doom tables.
// Node built-ins only; no package installation or network access is required.
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';

const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const serial = value => `${JSON.stringify(value, null, 2)}\n`;
const number = value => String(value).padStart(3, '0');
function write(file, content) {
  if (fs.existsSync(file) && fs.readFileSync(file, 'utf8') === content) return;
  const temporary = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(temporary, content);
  fs.renameSync(temporary, file);
}
function withoutComments(text) { return text.replace(/\/\*[\s\S]*?\*\/|\/\/[^\n]*/g, ''); }
const banner = file => `/* Generated from engine/${file} by tools/generate-data.mjs.
 * Original data: id Software / Simon Howard, GPL-2.0-or-later.
 * See ports/doom/LICENSE and the source copyright notice.
 * Edit the maintained engine source and regenerate. */\n`;

function tables(sourceFile, output) {
  const source = fs.readFileSync(sourceFile, 'utf8');
  const functions = [], prototypes = [], calls = {};
  const manifest = { format_version: 1, source_sha256: sha(source), chunk_elements: 64, tables: {}, sources: [] };
  const definitions = [['finetangent', 'fixed_t', 4096], ['finesine', 'fixed_t', 10240], ['tantoangle', 'angle_t', 2049]];
  for (const [name, type, count] of definitions) {
    const match = source.match(new RegExp(`const\\s+\\w+\\s+${name}\\s*\\[\\s*${count}\\s*\\]\\s*=\\s*\\{([\\s\\S]*?)\\}\\s*;`));
    if (!match) throw new Error(`cannot locate ${name}[${count}] initializer`);
    const values = withoutComments(match[1]).split(',').map(v => v.trim()).filter(Boolean).map(v => {
      const literal = v.replace(/[uUlL]+$/, '');
      if (!/^-?(?:0x[\da-f]+|\d+)$/i.test(literal)) throw new Error(`unsupported table literal: ${v}`);
      return literal.startsWith('-') ? -Number(literal.slice(1)) : Number(literal);
    });
    const lower = name === 'tantoangle' ? 0 : -0x80000000;
    const upper = name === 'tantoangle' ? 0xffffffff : 0x7fffffff;
    if (values.length !== count || values.some(v => !Number.isInteger(v) || v < lower || v > upper)) {
      throw new Error(`${name}: expected ${count} valid 32-bit values`);
    }
    const packed = Buffer.alloc(count * 4);
    values.forEach((v, index) => packed.writeUInt32LE(v >>> 0, index * 4));
    let fnv = 2166136261;
    for (const octet of packed) fnv = Math.imul(fnv ^ octet, 16777619) >>> 0;
    manifest.tables[name] = { entries: count, sha256_le32: sha(packed), fnv1a_le32: fnv.toString(16).padStart(8, '0') };
    calls[name] = [];
    const literal = v => type === 'angle_t' ? `UINT32_C(${v})` : v === -0x80000000 ? 'INT32_MIN' : v < 0 ? `(-INT32_C(${-v}))` : `INT32_C(${v})`;
    for (let index = 0; index < count; index += 64) {
      const symbol = `Doom_Fill_${name}_${number(index / 64)}`;
      prototypes.push(`${type} *${symbol}(${type} *out);`);
      const chunk = values.slice(index, index + 64);
      // Keep one base pointer for this bounded chunk. Reassigning out for every
      // element creates repeated fat-pointer spills and bank normalization.
      functions.push([`${type} *${symbol}(${type} *out)`, '{', ...chunk.map((v, n) => `    out[${n}] = ${literal(v)};`), `    return out + ${chunk.length};`, '}', ''].join('\n'));
      calls[name].push(`    cursor = ${symbol}(cursor);`);
    }
  }
  const header = `${banner('tables.c')}#include "tables.h"\n\n`;
  const generated = {};
  for (let index = 0; index < functions.length; index += 8) {
    generated[`tables_fill_${number(index / 8)}.c`] = header + functions.slice(index, index + 8).join('\n');
  }
  const driver = [header, '#include <stdlib.h>\n', ...prototypes, '', 'const fixed_t *finesine;', 'const fixed_t *finetangent;', 'const fixed_t *finecosine;', 'const angle_t *tantoangle;', ''];
  for (const [name, type] of definitions) {
    driver.push(`static void Init_${name}(void)`, '{', `    ${type} *cursor = (${type} *) ${name};`, ...calls[name], '    (void) cursor;', '}', '');
  }
  driver.push('void Doom_FreeTables(void)', '{', '    free((void *) finesine);', '    free((void *) finetangent);', '    free((void *) tantoangle);', '    finesine = NULL;', '    finetangent = NULL;', '    finecosine = NULL;', '    tantoangle = NULL;', '}', '',
    'int Doom_InitTables(void)', '{', '    if (finesine != NULL && finetangent != NULL && tantoangle != NULL)', '        return 1;', '    Doom_FreeTables();',
    '    finesine = (const fixed_t *) malloc((size_t) 10240 * sizeof(fixed_t));', '    finetangent = (const fixed_t *) malloc((size_t) 4096 * sizeof(fixed_t));', '    tantoangle = (const angle_t *) malloc((size_t) 2049 * sizeof(angle_t));',
    '    if (finesine == NULL || finetangent == NULL || tantoangle == NULL)', '    {', '        Doom_FreeTables();', '        return 0;', '    }',
    '    Init_finetangent();', '    Init_finesine();', '    Init_tantoangle();', '    finecosine = finesine + FINEANGLES / 4;', '    return 1;', '}', '');
  generated['tables_init.c'] = driver.join('\n');
  manifest.sources = Object.keys(generated).sort();
  for (const [file, content] of Object.entries(generated)) write(path.join(output, file), content);
  write(path.join(output, 'tables_manifest.json'), serial(manifest));
  return manifest;
}

function splitItems(text) {
  let depth = 0, start = 0;
  const result = [];
  for (let i = 0; i < text.length; i++) {
    if (text[i] === '{') depth++;
    if (text[i] === '}') { depth--; if (depth < 0) throw new Error('unbalanced initializer braces'); }
    if (text[i] === ',' && depth === 0) { result.push(text.slice(start, i).trim()); start = i + 1; }
  }
  if (depth !== 0) throw new Error('unbalanced initializer braces');
  if (text.slice(start).trim()) result.push(text.slice(start).trim());
  return result;
}

function info(sourceFile, output) {
  const source = fs.readFileSync(sourceFile, 'utf8');
  const cleaned = withoutComments(source);
  const stateFields = ['sprite', 'frame', 'tics', 'action.acv', 'nextstate', 'misc1', 'misc2'];
  const objectFields = ['doomednum', 'spawnstate', 'spawnhealth', 'seestate', 'seesound', 'reactiontime', 'attacksound', 'painstate', 'painchance', 'painsound', 'meleestate', 'missilestate', 'deathstate', 'xdeathstate', 'deathsound', 'speed', 'radius', 'height', 'mass', 'damage', 'activesound', 'flags', 'raisestate'];
  const definitions = [['states', 'state_t', 967, stateFields, 16], ['mobjinfo', 'mobjinfo_t', 137, objectFields, 4]];
  const functions = [], prototypes = [], calls = {}, callbacks = new Set();
  const manifest = { format_version: 1, source_sha256: sha(source), tables: {}, sources: [] };
  for (const [name, type, count, fields, chunk] of definitions) {
    const match = cleaned.match(new RegExp(`\\b\\w+\\s+${name}\\s*\\[[^\\]]+\\]\\s*=\\s*\\{([\\s\\S]*?)\\}\\s*;`));
    if (!match) throw new Error(`cannot locate ${name} initializer`);
    const records = splitItems(match[1]).map(record => {
      if (!record.startsWith('{') || !record.endsWith('}')) throw new Error(`${name}: expected record initializer`);
      const values = splitItems(record.slice(1, -1));
      if (values.length !== fields.length) throw new Error(`${name}: expected ${fields.length} fields, found ${values.length}`);
      if (name === 'states') {
        if (!/^\{\s*(NULL|A_\w+)\s*\}$/.test(values[3])) throw new Error('unsupported state action union initializer');
        values[3] = values[3].slice(1, -1).trim();
        if (values[3] !== 'NULL') callbacks.add(values[3]);
      }
      return values;
    });
    if (records.length !== count) throw new Error(`${name}: expected ${count} records, found ${records.length}`);
    manifest.tables[name] = { entries: count, fields, chunk_records: chunk, expressions_sha256: sha(JSON.stringify(records)) };
    calls[name] = [];
    for (let index = 0; index < count; index += chunk) {
      const symbol = `Doom_Fill_${name}_${number(index / chunk)}`;
      prototypes.push(`${type} *${symbol}(${type} *out);`);
      const body = [`${type} *${symbol}(${type} *out)`, '{'];
      for (const record of records.slice(index, index + chunk)) {
        fields.forEach((field, n) => body.push(`    out->${field} = ${record[n]};`));
        body.push('    ++out;');
      }
      body.push('    return out;', '}', '');
      functions.push(body.join('\n'));
      calls[name].push(`    cursor = ${symbol}(cursor);`);
    }
  }
  const actions = [...callbacks].sort();
  write(path.join(output, 'info_actions.h'), `#ifndef DOOM_INFO_ACTIONS_H\n#define DOOM_INFO_ACTIONS_H\n${actions.map(name => `void ${name}();\n`).join('')}#endif\n`);
  write(path.join(output, 'info_test_actions.h'), `/* Test-only stable callback identities. */\n#define DOOM_INFO_ACTIONS(X) \\\n${actions.map((name, n) => `    X(${n + 1}, ${name})`).join(' \\\n')}\n`);
  const header = `${banner('info.c')}#include <stddef.h>\n#include "info.h"\n#include "sounds.h"\n#include "m_fixed.h"\n#include "p_mobj.h"\n#include "info_actions.h"\n\n`;
  const generated = {};
  for (let index = 0; index < functions.length; index += 4) generated[`info_fill_${number(index / 4)}.c`] = header + functions.slice(index, index + 4).join('\n');
  const driver = [header, '#include <stdlib.h>\n', ...prototypes, '', 'state_t *states;', 'mobjinfo_t *mobjinfo;', ''];
  for (const [name, type] of definitions) driver.push(`static void Init_${name}(void)`, '{', `    ${type} *cursor = ${name};`, ...calls[name], '    (void) cursor;', '}', '');
  driver.push('void Doom_FreeInfo(void)', '{', '    free(states);', '    free(mobjinfo);', '    states = NULL;', '    mobjinfo = NULL;', '}', '',
    'int Doom_InitInfo(void)', '{', '    if (states != NULL && mobjinfo != NULL) return 1;', '    Doom_FreeInfo();', '    states = malloc((size_t) NUMSTATES * sizeof(*states));', '    mobjinfo = malloc((size_t) NUMMOBJTYPES * sizeof(*mobjinfo));',
    '    if (states == NULL || mobjinfo == NULL)', '    {', '        Doom_FreeInfo();', '        return 0;', '    }', '    Init_states();', '    Init_mobjinfo();', '    return 1;', '}', '');
  generated['info_init.c'] = driver.join('\n');
  manifest.sources = Object.keys(generated).sort();
  manifest.actions = actions;
  for (const [file, content] of Object.entries(generated)) write(path.join(output, file), content);
  write(path.join(output, 'info_manifest.json'), serial(manifest));
  return manifest;
}

try {
  const [engine, output] = process.argv.slice(2);
  if (!engine || !output) throw new Error('usage: generate-data.mjs ENGINE_DIRECTORY OUTPUT_DIRECTORY');
  fs.mkdirSync(output, { recursive: true });
  const tableManifest = tables(path.join(engine, 'tables.c'), output);
  const infoManifest = info(path.join(engine, 'info.c'), output);
  const owned = new Set([...tableManifest.sources, ...infoManifest.sources]);
  for (const file of fs.readdirSync(output)) {
    if (/^(tables|info)_fill_\d+\.c$/.test(file) && !owned.has(file)) fs.unlinkSync(path.join(output, file));
  }
  const inputs = sha(tableManifest.source_sha256 + infoManifest.source_sha256 + sha(fs.readFileSync(new URL(import.meta.url))));
  write(path.join(output, 'sources.mk'), `# Generated from engine/tables.c and engine/info.c.\n# Inputs SHA256: ${inputs}\nTABLE_SOURCES := ${tableManifest.sources.map(name => path.join(output, name)).join(' ')}\nINFO_SOURCES := ${infoManifest.sources.map(name => path.join(output, name)).join(' ')}\n`);
} catch (error) { console.error(`error: ${error.message}`); process.exitCode = 1; }
