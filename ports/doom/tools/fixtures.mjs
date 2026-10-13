#!/usr/bin/env node
// Extract actual engine routines, keeping isolated regressions tied to production.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const port = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const [program, directory] = process.argv.slice(2);
if (!directory) throw new Error('usage: fixtures.mjs PROGRAM OUTPUT_DIRECTORY');
const out = path.resolve(directory);
fs.mkdirSync(out, { recursive: true });
const read = name => fs.readFileSync(path.join(port, 'engine', name), 'utf8');
function before(text, marker) {
  const end = text.indexOf(marker);
  if (end < 0) throw new Error('missing engine fixture marker: ' + marker);
  return text.slice(0, end);
}
function from(text, marker) {
  const start = text.indexOf(marker);
  if (start < 0) throw new Error('missing engine fixture marker: ' + marker);
  return text.slice(start);
}
function write(name, content) {
  const target = path.join(out, name);
  if (!fs.existsSync(target) || fs.readFileSync(target, 'utf8') !== content)
    fs.writeFileSync(target, content);
}
if (program === 'projection_widths') {
  const source = read('r_segs.c');
  write('projection_loop.inc', before(source, '//\n// R_RenderMaskedSegRange')
    + before(from(source, '#define HEIGHTBITS'), '//\n// R_StoreWallRange'));
} else if (program === 'texture_columns') {
  write('texture_cache.inc', before(read('r_data.c'), 'static void GenerateTextureHashTable(void)'));
} else if (program === 'config_bindings') {
  write('config_bindings.inc', before(from(read('d_main.c'), 'void D_BindVariables(void)'),
    '//\n// D_GrabMouseCallback'));
} else if (program === 'keyboard_controls') {
  const source = read('g_game.c');
  write('input_commands.inc', before(source, 'int G_CmdChecksum')
    + '\nstatic int G_NextWeapon(int direction);\n'
    + before(from(source, 'void G_BuildTiccmd'), 'void G_DoLoadLevel (void)')
    + before(from(source, 'static void SetJoyButtons'), 'void G_Ticker (void)'));
  write('controls_defaults.inc', before(read('m_controls.c'), 'void M_BindBaseControls(void)'));
  const platform = fs.readFileSync(path.join(port, 'src/platform.c'), 'utf8');
  write('platform_keyboard.inc', before(from(platform, 'static unsigned char held['), ';')
    + ';\n' + before(from(platform, 'static int doom_key('), 'void I_StartTic(void)'));
} else throw new Error('unknown extracted fixture: ' + program);
