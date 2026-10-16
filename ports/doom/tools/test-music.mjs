#!/usr/bin/env node
// Music is synthesized by the shared host device, not the Clang pixel oracle.
import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import {PORT, BUILD, WAD, RVM, BANK, MEMORY, run, prepareDisk, nativeInput, mkdir} from './test-support.mjs';
const out = path.join(BUILD, 'tests/music');
mkdir(out);
for (const target of ['both', 'rcc-native']) {
  run(`build-${target}`, ['make', '-C', PORT, '-j4', `TARGET=${target}`, 'PROGRAM=engine',
    `BUILD=${BUILD}`, 'START_MAP=0', 'START_DEMO=0', 'PCM_AUDIO=1', 'MUSIC_AUDIO=1'], out, 900);
}
const disk = path.join(out, 'doom1.wad');
prepareDisk(WAD, disk, {wadMode: 'cooked', manifest: path.join(BUILD, 'manifest.json')});
const titleInput = path.join(out, 'title.json');
fs.writeFileSync(titleInput, '[]');
const gameplayInput = path.join(out, 'gameplay.json');
fs.writeFileSync(gameplayInput, JSON.stringify([
  ...JSON.parse(fs.readFileSync(path.join(PORT, 'tests/menu-start.json'), 'utf8')),
  {tick: 12, key: 224, down: true}, {tick: 40, key: 224, down: false},
]));
for (const [scene, input, frame] of [
  ['title', titleInput, 12], ['gameplay', gameplayInput, 40],
  ['music-only', path.join(PORT, 'tests/menu-start.json'), 40],
]) {
  const sceneOut = path.join(out, scene); mkdir(sceneOut);
  const textInput = path.join(sceneOut, 'input.txt'); nativeInput(input, textInput);
  const expected = path.join(sceneOut, 'reference.rgb565');
  const log = run('reference', [path.join(BUILD, 'native/engine/engine'), expected, frame, 'title', textInput], sceneOut, 120);
  const ticks = [...log.matchAll(/ENGINE frame \d+ tick (\d+)/g)];
  const finalTick = Number(ticks.at(-1)[1]);
  let reference;
  for (const engine of ['interpreter', 'jit', 'native']) {
    const wave = path.join(sceneOut, `${engine}.wav`), pixels = path.join(sceneOut, `${engine}.rgb565`);
    const command = engine === 'native'
      ? ['env', 'RCC_NATIVE_HEADLESS=1', 'RCC_NATIVE_UNTHROTTLED=1', 'RCC_NATIVE_AUDIO=off',
        `RCC_NATIVE_AUDIO_WAV=${wave}`, `RCC_NATIVE_FRAME=${pixels}`, `RCC_NATIVE_FRAME_NUMBER=${frame}`,
        `RCC_NATIVE_INPUT_SCRIPT=${input}`, 'RCC_NATIVE_TIMEOUT=120', path.join(BUILD, 'rcc-native/engine/engine')]
      : [RVM, path.join(BUILD, 'ripple/engine/engine.bin'), '--headless', '--bank-size', BANK, '--memory', MEMORY,
        '--disk', disk, '--clock', 'unthrottled', '--audio', 'off', '--audio-wav', wave, '--input-script', input,
        '--capture-frame', frame, '--raw-frame', pixels, '--max-instructions', '2000000000', '--timeout', '120',
        ...(engine === 'jit' ? ['--jit'] : [])];
    run(engine, command, sceneOut, 130);
    const wav = fs.readFileSync(wave);
    assert.equal(wav.toString('ascii', 0, 4), 'RIFF');
    assert.equal(wav.readUInt32LE(24), 22050); assert.equal(wav.readUInt16LE(22), 2);
    assert.equal(wav.length, 44 + finalTick * 630 * 4);
    assert(wav.subarray(44).some(value => value !== 0), `${scene}/${engine}: missing music`);
    if (reference) assert.deepEqual(wav, reference, `${scene}/${engine}: PCM mismatch`);
    else reference = wav;
    assert.deepEqual(fs.readFileSync(pixels), fs.readFileSync(expected), `${scene}/${engine}: pixel mismatch`);
    console.log(`${scene}/${engine}: music PCM matches across backends; pixels match Clang`);
  }
}
assert.notDeepEqual(fs.readFileSync(path.join(out, 'gameplay/interpreter.wav')),
  fs.readFileSync(path.join(out, 'music-only/interpreter.wav')), 'gunfire must remain audible in the music mix');
console.log(`Doom music checks passed: ${out}`);
