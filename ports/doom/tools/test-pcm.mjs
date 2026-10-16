#!/usr/bin/env node
// Compare actual game PCM and pixels with the independent host C mixer.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import {PORT, BUILD, WAD, RVM, BANK, MEMORY, run, prepareDisk, nativeInput, mkdir} from './test-support.mjs';

const out = path.join(BUILD, 'tests/pcm');
mkdir(out);
for (const target of ['both', 'rcc-native']) {
  run(`build-${target}`, ['make', '-C', PORT, '-j4', `TARGET=${target}`, 'PROGRAM=engine',
    `BUILD=${BUILD}`, 'START_MAP=0', 'START_DEMO=0', 'MUSIC_AUDIO=0'], out, 900);
}
const disk = path.join(out, 'doom1.wad');
prepareDisk(WAD, disk, {wadMode: 'cooked', manifest: path.join(BUILD, 'manifest.json')});
const gameplayInput = path.join(out, 'gameplay.json');
fs.writeFileSync(gameplayInput, JSON.stringify([
  ...JSON.parse(fs.readFileSync(path.join(PORT, 'tests/menu-start.json'), 'utf8')),
  {tick: 12, key: 224, down: true},
  {tick: 40, key: 224, down: false},
]));

for (const [scene, input, frame] of [
  ['menu', path.join(PORT, 'tests/menu-open.json'), 4],
  ['gameplay', gameplayInput, 40],
]) {
  const sceneOut = path.join(out, scene);
  mkdir(sceneOut);
  const textInput = path.join(sceneOut, 'input.txt');
  nativeInput(input, textInput);
  const expected = path.join(sceneOut, 'reference.rgb565');
  const raw = path.join(sceneOut, 'reference.pcm');
  const log = run('reference', ['env', `DOOM_PCM_RAW=${raw}`, path.join(BUILD, 'native/engine/engine'),
    expected, frame, 'title', textInput], sceneOut, 120);
  const ticks = [...log.matchAll(/ENGINE frame \d+ tick (\d+)/g)];
  const finalTick = Number(ticks.at(-1)[1]);
  const pcm = fs.readFileSync(raw);
  assert.equal(pcm.length, finalTick * 630 * 2 * 2);
  assert(pcm.some(value => value !== 0), `${scene}: replay must produce sound`);
  if (scene === 'gameplay') {
    assert.match(log, /ENGINE frame 40 tick \d+ state 0 episode 1 map 1/);
    // Allow the initial weapon-raising animation to finish before checking fire.
    assert(pcm.subarray(30 * 630 * 4).some(value => value !== 0), 'gameplay must produce sound after entering E1M1');
  }
  for (const engine of ['interpreter', 'jit', 'native']) {
    const wave = path.join(sceneOut, `${engine}.wav`);
    const pixels = path.join(sceneOut, `${engine}.rgb565`);
    const command = engine === 'native'
      ? ['env', 'RCC_NATIVE_HEADLESS=1', 'RCC_NATIVE_UNTHROTTLED=1', 'RCC_NATIVE_AUDIO=off',
        `RCC_NATIVE_AUDIO_WAV=${wave}`, `RCC_NATIVE_FRAME=${pixels}`, `RCC_NATIVE_FRAME_NUMBER=${frame}`,
        `RCC_NATIVE_INPUT_SCRIPT=${input}`, 'RCC_NATIVE_TIMEOUT=120', path.join(BUILD, 'rcc-native/engine/engine')]
      : [RVM, path.join(BUILD, 'ripple/engine/engine.bin'), '--headless', '--bank-size', BANK,
        '--memory', MEMORY, '--disk', disk, '--clock', 'unthrottled', '--audio', 'off', '--audio-wav', wave,
        '--input-script', input, '--capture-frame', frame, '--raw-frame', pixels,
        '--max-instructions', '2000000000', '--timeout', '120', ...(engine === 'jit' ? ['--jit'] : [])];
    run(engine, command, sceneOut, 130);
    const wav = fs.readFileSync(wave);
    assert.equal(wav.toString('ascii', 0, 4), 'RIFF');
    assert.equal(wav.readUInt32LE(24), 22050);
    assert.equal(wav.readUInt16LE(22), 2);
    assert.deepEqual(wav.subarray(44), pcm, `${scene}/${engine}: game PCM differs from host C`);
    assert.deepEqual(fs.readFileSync(pixels), fs.readFileSync(expected), `${scene}/${engine}: game pixels differ`);
    console.log(`${scene}/${engine}: exact game PCM and pixels match the host C oracle`);
  }
}
console.log(`Doom PCM checks passed: ${out}`);
