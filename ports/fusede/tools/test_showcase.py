#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real RVM/Fuse-X image protocol, pixel and showcase checks on private disks."""
import argparse
import json
import os
from pathlib import Path
import re
import select
import struct
import subprocess
import sys
import tempfile
import time
from build_theme import ROOT, png, unpack565


def reference(assets):
    result = [0x1905] * (128 * 96)
    for y in range(4, 13):
        for x in range(48):
            sx, sy = (x + 3) // 3, (y - 4) // 3
            if (sx + sy) % 3: result[y * 128 + x] = 0x8000 + sy * 17 + sx
    meta = json.loads((assets / 'theme.json').read_text())
    data = (assets / 'atlas.rgb565').read_bytes()
    pixels = struct.unpack('<' + 'H' * (len(data) // 2), data)
    data = (assets / 'atlas.mask').read_bytes()
    mask = struct.unpack('<' + 'H' * (len(data) // 2), data)
    asset = meta['assets']['button-normal']
    ax, ay, sw, sh = asset['rect']; left, top, right, bottom = asset['insets']
    def sample(pos, size, source, first, last):
        if pos < first: return pos
        if pos >= size - last: return source - last + pos - (size - last)
        return first + (pos - first) * (source - first - last) // (size - first - last)
    # Clip the stretched skin with a user GC region, preserving sampling phase.
    for y in range(30, 50):
        for x in range(12, 102):
            sx = ax + sample(x - 8, 101, sw, left, right)
            sy = ay + sample(y - 28, 27, sh, top, bottom)
            if mask[sy * meta['mask_row_words'] + sx // 16] & (0x8000 >> (sx % 16)):
                result[y * 128 + x] = pixels[sy * meta['width'] + sx]
    icons = json.loads((ROOT / 'ports/fusede/icons/masks.json').read_text())
    folder = icons['icons']['folder']['24']
    for y in range(24):
        for x in range(24):
            if folder[y * 2 + x // 16] & (0x8000 >> (x % 16)):
                result[(62 + y) * 128 + 12 + x] = 0x5433
    for y in range(80, 88):
        for x in range(110, 118): result[y * 128 + x] = 0x5433
    return result


def compare(got, expected, label):
    if len(got) != len(expected): raise AssertionError(f'{label}: wrong pixel count {len(got)}')
    for i, (a, b) in enumerate(zip(got, expected)):
        if a != b: raise AssertionError(f'{label}: pixel ({i%128},{i//128}) {a:04x} != {b:04x}')


def save_png(path, width, height, words):
    png(path, width, height, [(*unpack565(p), 255) for p in words])


def rvm(build, output):
    with tempfile.TemporaryDirectory(prefix='fusede-rvm-') as directory:
        image = Path(directory) / 'root.img'
        subprocess.run(['bun', str(ROOT / 'ports/fusede/tools/test_image.mjs'), str(build), str(image)], check=True)
        subprocess.run(['cargo', 'test', '--release', '-p', 'rvm', '--test', 'fusede', '--', '--ignored', '--nocapture'],
                       cwd=ROOT, check=True, env=dict(os.environ, FDE_RVM_BUILD=str(build), FDE_TEST_DISK=str(image),
                                                      FDE_TEST_OUTPUT=str(output)))
        data = (output / 'rvm-check.rgb565').read_bytes()
        words = struct.unpack('<' + 'H' * (len(data) // 2), data)
        got = [words[(20 + y) * 640 + 20 + x] for y in range(96) for x in range(128)]
        compare(got, reference(build / 'fusede'), 'RVM')
        save_png(output / 'rvm-pixels.png', 128, 96, got)
        data = (output / 'rvm-showcase.rgb565').read_bytes()
        save_png(output / 'rvm-showcase.png', 640, 480, struct.unpack('<' + 'H' * (len(data) // 2), data))
        subprocess.run([sys.executable, str(ROOT / 'ports/fuzix/tools/fsck.py'), str(image)], check=True)
    print('RVM: all 12,288 probe pixels match the independent oracle')


def native(build, output):
    with tempfile.TemporaryDirectory(prefix='fusede-native-') as directory:
        work = Path(directory); apps = work / 'apps'; apps.mkdir()
        (work / 'fusede').symlink_to(build / 'fusede', target_is_directory=True)
        names = ['native-hello','cat','ls','cp','sh','kill','echo','sleep','nano-X','fdeshowcase','nxhostcheck','nxcheck','fdectl']
        for name in names: (apps / f'{name}.fxe').symlink_to(build / 'apps' / f'{name}.fxe')
        (work / 'application-report.json').write_text(json.dumps([dict(name=n,status='built') for n in names]))
        image = work / 'root.img'; frames = work / 'frames'
        subprocess.run(['bun', str(ROOT / 'fuse-x/image.mjs'), 'create', str(image), '--apps', str(apps)], check=True)
        env = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off', RCC_NATIVE_TIMEOUT='120',
                   FUSE_X_FRAMES=str(frames), NANOX_KEEP_TTY='1')
        child = subprocess.Popen([str(build / 'fuse-x'), '--run', str(image), '--one-shot'],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        log = bytearray()
        def until(marker, start=0):
            deadline = time.monotonic() + 45
            while marker.encode() not in log[start:]:
                if time.monotonic() > deadline: raise AssertionError(f'timeout {marker}\n{log.decode(errors="replace")}')
                if select.select([child.stdout], [], [], .1)[0]:
                    data = os.read(child.stdout.fileno(), 65536)
                    if not data: raise AssertionError(log.decode(errors='replace'))
                    log.extend(data)
        def command(value, marker=None):
            start = len(log);child.stdin.write((value+'\n').encode());child.stdin.flush()
            if marker: until(marker, start)
            until('fuse-x# ', start)
            return log[start:].decode(errors='replace')
        def frame(title, size=None):
            deadline=time.monotonic()+15
            while time.monotonic()<deadline:
                try:
                    windows=json.loads((frames/'windows.json').read_text())
                    found=next(w for w in windows if w['title']==title and w['visible'])
                    path=frames/f"{found['id']}.ppm";data=path.read_bytes()
                    match=re.match(rb'P6\n(\d+) (\d+)\n255\n',data)
                    width,height=map(int,match.groups())
                    pixels=data[match.end():]
                    if len(pixels)==width*height*3 and (size is None or (width,height)==size):
                        return width,height,pixels,path
                except (FileNotFoundError,StopIteration,json.JSONDecodeError,AttributeError): pass
                time.sleep(.02)
            raise AssertionError(f'missing native frame {title}\n{log.decode(errors="replace")}')
        try:
            until('fuse-x# ')
            command('NANOX_KEEP_TTY=1');command('export NANOX_KEEP_TTY')
            command('FDE_TRACE=1');command('export FDE_TRACE')
            command('nano-X -p &');command('s=$!');until('Nano-X ready')
            command('fdeshowcase --check &');command('p=$!');until('FDE_IMAGE_CHECKS_OK')
            expected=reference(build/'fusede')
            # Frame writes and the application marker are on independent streams.
            deadline=time.monotonic()+10
            while True:
                w,h,rgb,_=frame('FuseDE pixel check',(128,96))
                got=[(rgb[i]>>3)<<11 | (rgb[i+1]>>2)<<5 | rgb[i+2]>>3 for i in range(0,len(rgb),3)]
                if got==expected: break
                if time.monotonic()>deadline: compare(got,expected,'Fuse-X')
                time.sleep(.03)
            save_png(output/'fuse-x-pixels.png',w,h,got)
            command('kill -10 $p');command('wait $p; echo FDE_NATIVE_CHECK:$?','FDE_NATIVE_CHECK:0')
            command('nxcheck; echo FDE_NATIVE_NX:$?','FDE_NATIVE_NX:0')
            command('fdectl --check; echo FDE_NATIVE_WORKSPACE:$?', 'FDE_NATIVE_WORKSPACE:0')
            text=command('fdeshowcase &','FDE_SHOWCASE_READY');command('a=$!')
            wid=int(re.search(r'FDE_SHOWCASE_READY (\d+)',text).group(1))
            w,h,rgb,path=frame('FuseDE Showcase',(560,400))
            # Wait for the first full repaint before capturing the showcase.
            time.sleep(.2); w,h,rgb,path=frame('FuseDE Showcase',(560,400))
            (output/'fuse-x-showcase.ppm').write_bytes(path.read_bytes())
            png(output/'fuse-x-showcase.png',w,h,[(*rgb[i:i+3],255) for i in range(0,len(rgb),3)])
            command(f'nxhostcheck ctl resize {wid} 640 440','ROOTLESS_CONTROL_OK')
            frame('FuseDE Showcase',(640,440))
            command(f'nxhostcheck ctl close {wid}','ROOTLESS_CONTROL_OK')
            until('FDE_SHOWCASE_CLOSED');command('wait $a; echo FDE_NATIVE_APP:$?','FDE_NATIVE_APP:0')
            command('nxcheck; echo FDE_NATIVE_REOPEN:$?','FDE_NATIVE_REOPEN:0')
            command('kill $s');command('wait $s')
            child.stdin.write(b'shutdown\n');child.stdin.flush();until('filesystem synced; session stopped')
            if child.wait(timeout=5)!=0: raise AssertionError(log.decode(errors='replace'))
            if any(s in log for s in (b'panic:',b'failed line',b'NXCHECK_',b'GsError')): raise AssertionError(log.decode(errors='replace'))
        finally:
            if child.poll() is None:
                child.terminate()
                try: child.wait(timeout=5)
                except subprocess.TimeoutExpired: child.kill();child.wait()
            (output/'fuse-x.log').write_bytes(log)
        check=("import {checkRoot} from './ports/fuzix/tools/fsck.mjs';"
               "import {openImage} from './ports/fuzix/tools/image.mjs';"
               "const r=checkRoot(openImage(process.argv[1]));if(r.issues.length)throw Error(r.issues.join('; '));")
        subprocess.run(['bun','--input-type=module','-e',check,str(image)],cwd=ROOT,check=True)
    print('Fuse-X: pixels, independent window resize/close, server reuse and filesystem checks passed')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os',choices=['rvm','fuse-x','both'],default='both')
    parser.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
    parser.add_argument('--native-build',type=Path,default=ROOT/'fuse-x/build')
    parser.add_argument('--output',type=Path,default=ROOT/'build/fusede/tests')
    args=parser.parse_args();args.output=args.output.resolve();args.output.mkdir(parents=True,exist_ok=True)
    if args.os in ('rvm','both'): rvm(args.build.resolve(),args.output)
    if args.os in ('fuse-x','both'): native(args.native_build.resolve(),args.output)
