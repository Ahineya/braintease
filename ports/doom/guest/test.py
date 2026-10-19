#!/usr/bin/env python3
"""Boot a disposable Fuse-X root, run Doom, and check real GX frames/cleanup."""
import argparse
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import tempfile
import time
import wave

ROOT = Path(__file__).resolve().parents[3]
DOOM = ROOT / 'ports/doom'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=DOOM / 'build/guest')
    parser.add_argument('--kernel', type=Path, default=ROOT / 'fuse-x/build/fuse-x')
    parser.add_argument('--reference', type=Path, help='Clang reference engine for exact pixel checks')
    args = parser.parse_args()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='doom-guest-') as temporary:
        work = Path(temporary)
        share = work / 'share'
        share.mkdir()
        shutil.copy2(build / 'fuse-x/doom.fxe', share / 'doom.fxe')
        shutil.copy2(DOOM / 'assets/doom1.wad', share / 'doom1.wad')
        image = work / 'root.img'
        # Make a small new filesystem from source assets, never a personal disk.
        fixture = '''
import fs from 'node:fs';
import {rootImage} from './ports/fuzix/tools/disk.mjs';
import {writeImage} from './ports/fuzix/tools/image.mjs';
const [image,shell]=process.argv.slice(1);
writeImage(image,rootImage([
 {name:'/init',mode:0o100755,data:Buffer.from('FXN1:init:1\\n')},
 {name:'/bin/sh',mode:0o100755,data:fs.readFileSync(shell)},
 {name:'/bin/echo',mode:0o100755,data:fs.readFileSync(shell.replace(/sh\\.fxe$/, 'echo.fxe'))},
 {name:'/dev/gfx',mode:0o20666,device:0x400},
 {name:'/dev/pcm',mode:0o20666,device:0x500},
 {name:'/mnt/host',directory:true,mode:0o40755},
 {name:'/etc/passwd',mode:0o100644,data:Buffer.from('root:x:0:0:root:/:/bin/sh\\n')}
],32768),true);
'''
        subprocess.run(['node', '--input-type=module', '-e', fixture, str(image),
                        str(ROOT / 'fuse-x/build/apps/sh.fxe')], cwd=ROOT, check=True)
        frame = work / 'frame.ppm'
        registry = work / 'frames/windows.json'
        event = work / 'event.json'
        audio = work / 'audio.wav'
        env = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off',
                   RCC_NATIVE_TIMEOUT='240', FUSE_X_FRAME=str(frame),
                   FUSE_X_FRAMES=str(registry.parent), FUSE_X_TEST_EVENT=str(event),
                   RCC_NATIVE_AUDIO='off', RCC_NATIVE_AUDIO_WAV=str(audio))
        child = subprocess.Popen([str(args.kernel.resolve()), '--run', str(image), '--share', str(share), '--one-shot'],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        log = bytearray()

        def until(marker, start=0, timeout=60):
            deadline = time.monotonic() + timeout
            while marker.encode() not in log[start:]:
                if time.monotonic() > deadline:
                    raise AssertionError(f'Timeout waiting for {marker!r}:\n{log.decode(errors="replace")}')
                if select.select([child.stdout], [], [], .1)[0]:
                    data = os.read(child.stdout.fileno(), 65536)
                    if not data:
                        raise AssertionError(f'Kernel exited {child.poll()}:\n{log.decode(errors="replace")}')
                    log.extend(data)

        def send(command):
            child.stdin.write((command + '\n').encode())
            child.stdin.flush()

        def command(command):
            start = len(log)
            send(command + '; echo DOOM_STATUS:$?')
            until('DOOM_STATUS:', start)
            until('fuse-x# ', start)
            output = log[start:].decode(errors='replace')
            assert '\r\nDOOM_STATUS:0\r\n' in output, output
            return output

        def windows():
            return json.loads(registry.read_text()) if registry.exists() else []

        def pixels(reference):
            raw = reference.read_bytes()
            result = bytearray(b'P6\n960 600\n255\n')
            row = bytearray()
            for i in range(0, len(raw), 2):
                value = raw[i] | raw[i+1] << 8
                r, g, b = value >> 11, (value >> 5) & 63, value & 31
                row.extend(bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))) * 3)
                if i % 640 == 638:
                    result.extend(row * 3)
                    row.clear()
            return result

        try:
            until('fuse-x# ')
            command('/mnt/host/doom.fxe --help')
            for scene, options in [('title', ''), ('map', '-warp 1 1'), ('demo1', '-iwaddemo demo1')]:
                output = command('/mnt/host/doom.fxe -nosound --fast --frames 32 ' + options)
                assert 'DOOM_GUEST_OK frames=32' in output, output
                assert not windows(), f'Leaked graphics window: {windows()}'
                capture = build / f'{scene}.ppm'
                capture.write_bytes(frame.read_bytes())
                assert capture.read_bytes().startswith(b'P6\n960 600\n255\n')
                assert len(set(capture.read_bytes()[16:])) > 20, 'Blank Doom screen'
                if args.reference:
                    reference = build / f'{scene}.rgb565'
                    subprocess.run([str(args.reference.resolve()), str(reference), '32', scene], check=True,
                                   stdout=subprocess.DEVNULL)
                    assert capture.read_bytes() == pixels(reference), f'{scene} pixels differ from reference'
                print(f'Passed {scene}: 32 frames and window cleanup', flush=True)
            # Exercise the paced path and a real native-window close event.
            start = len(log)
            send('/mnt/host/doom.fxe -nosound -warp 1 1; echo DOOM_STATUS:$?')
            deadline = time.monotonic() + 30
            while not any(w['title'] == 'Doom' and w['sequence'] >= 3 for w in windows()):
                assert time.monotonic() < deadline, 'No running Doom window'
                time.sleep(.05)
            window = next(w for w in windows() if w['title'] == 'Doom')
            assert window['resizable'] and (window['width'], window['height']) == (960, 600)
            for width, height in [(800, 600), (1280, 720), (320, 200), (1920, 1080)]:
                temporary = event.with_suffix('.tmp')
                temporary.write_text(json.dumps(dict(window=window['id'], resize=[width, height])))
                temporary.replace(event)
                deadline = time.monotonic() + 10
                while True:
                    current = next(w for w in windows() if w['id'] == window['id'])
                    if (current['width'], current['height']) == (width, height):
                        break
                    assert time.monotonic() < deadline, f'Resize not acknowledged: {current}'
                    time.sleep(.05)
                # Configure first publishes a resized old frame. Wait for a
                # later game frame before checking the letterboxed image.
                sequence = current['sequence']
                while next(w for w in windows() if w['id'] == window['id'])['sequence'] <= sequence:
                    assert time.monotonic() < deadline
                    time.sleep(.05)
                captured = (registry.parent / f'{window["id"]}.ppm').read_bytes()
                header, data = captured.split(b'\n255\n', 1)
                assert header == f'P6\n{width} {height}'.encode()
                assert len(data) == width * height * 3
                if width * 200 != height * 320:
                    assert data[:3] == b'\0\0\0' and data[-3:] == b'\0\0\0', 'Letterbox border is not black'
            print('Passed default 3x size, resizing, fullscreen-sized geometry and letterboxing', flush=True)
            temporary = event.with_suffix('.tmp')
            temporary.write_text(json.dumps(dict(window=window['id'], close=True)))
            temporary.replace(event)
            until('DOOM_STATUS:0', start)
            until('fuse-x# ', start)
            assert not windows(), 'Window close leaked its graphics lease'
            # Music-only output distinguishes MUS/OPL3 from PCM sound effects.
            output = command('/mnt/host/doom.fxe -nosfx -warp 1 1 --frames 75')
            assert 'Doom OPL3 music enabled' in output, output
            with wave.open(str(audio), 'rb') as wav:
                assert (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) == (22050, 2, 2)
                music = wav.readframes(wav.getnframes())
                assert len(music) > 22050 * 4 and len(set(music)) > 32, 'Missing or silent OPL3 output'
            shutil.copy2(audio, build / 'music.wav')
            before = len(music)
            command('/mnt/host/doom.fxe -nomusic -iwaddemo demo1 --frames 140')
            with wave.open(str(audio), 'rb') as wav:
                effects = wav.readframes(wav.getnframes())[before:]
                assert len(effects) > 22050 * 4 and len(set(effects)) > 32, 'Missing or silent PCM effects'
            shutil.copy2(audio, build / 'audio.wav')
            assert not windows()
            print('Passed audible OPL3 music, separate PCM effects, repeated audio ownership and cleanup', flush=True)
            start = len(log)
            send('/mnt/host/doom.fxe -iwad /missing.wad; echo MISSING_STATUS:$?')
            until('MISSING_STATUS:1', start)
            until('fuse-x# ', start)
            assert not windows()
            send('shutdown')
            child.wait(timeout=20)
            assert child.returncode == 0
        finally:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=10)
            (build / 'fuse-x-test.log').write_bytes(log)
    print('Doom guest: title, E1M1, demo, paced playback, close, missing WAD and cleanup passed')


if __name__ == '__main__':
    main()
