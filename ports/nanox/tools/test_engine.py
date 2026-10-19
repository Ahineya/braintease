#!/usr/bin/env python3
"""Run the RCC engine on JIT RVM and compare every displayed pixel independently."""
import argparse
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
NX = ROOT / 'ports/nanox'
WIDTH, HEIGHT = 640, 480


def reference():
    pixels = [0] * (WIDTH * HEIGHT)
    source = (NX / 'upstream/src/fonts/compiled/rom8x16.c').read_text(encoding='latin1')
    body = source.split('rom8x16_bits[] = {', 1)[1].split('};', 1)[0]
    font = [int(n, 16) for n in re.findall(r'0x[\da-fA-F]+', re.sub(r'/\*.*?\*/', '', body, flags=re.S))]

    def visible(x, y):
        return 0 <= x < WIDTH and 0 <= y < HEIGHT

    clip = visible

    def point(x, y, value):
        if clip(x, y):
            pixels[y * WIDTH + x] = value

    def box(x, y, w, h, value):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                point(xx, yy, value)

    def text(x, y, value, colour):
        for char in value:
            for row in range(16):
                bits = font[ord(char) * 16 + row]
                for col in range(8):
                    if bits & (0x8000 >> col):
                        point(x + col, y + row, colour)
            x += 8

    box(0, 0, 640, 480, 0x1082)
    box(0, 0, 640, 64, 0x18e3)
    text(24, 16, 'Nano-X on Ripple', 0xffff)
    text(24, 40, 'Drawing engine / RGB565', 0x7e7f)
    box(24, 88, 280, 184, 0x2104)
    text(40, 104, 'LINES + COLOUR', 0xffff)
    for i, colour in enumerate([0xf800, 0xfd20, 0xffe0, 0x07e0, 0x07ff, 0x001f, 0x801f, 0xffff]):
        box(40 + i * 30, 136, 24, 24, colour)
    for i in range(7):
        box(40, 184 + i * 10, 225, 1, 0x0410 + i * 0x1000)
    box(328, 88, 288, 184, 0x2104)
    text(344, 104, 'REGIONS + CLIPPING', 0xffff)
    clip = lambda x, y: visible(x, y) and 344 <= x < 600 and 136 <= y < 248 and not (384 <= x < 456 and 169 <= y < 225)
    for i in range(12):
        box(328, 128 + i * 12, 288, 8, 0x05d7 if i & 1 else 0x03ef)
    text(332, 170, 'THIS TEXT IS CLIPPED', 0xffe0)
    clip = visible
    box(24, 292, 592, 140, 0x2104)
    text(40, 308, 'BITMAPS + OVERLAPPING BLITS', 0xffff)
    bitmap = [0xaaaa, 0x8000, 0x5555, 0, 0xffff, 0x8000, 0x8000, 0x8000]
    for y in range(4):
        for x in range(17):
            point(40 + x, 344 + y, 0xffe0 if bitmap[y * 2 + x // 16] & (0x8000 >> (x % 16)) else 0x001f)
    for i in range(16):
        box(80 + i * 8, 344, 8, 48, 0x2000 + i * 0x0801)
    saved = pixels.copy()
    for y in range(40):
        for x in range(112):
            point(96 + x, 352 + y, saved[(344 + y) * WIDTH + 80 + x])
    text(240, 344, '8x16 compiled font', 0x7e7f)
    text(240, 372, 'One pixel = one C16 cell', 0xffff)
    text(24, 452, 'Esc / Q to close', 0x9cf3)
    return pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
    args = parser.parse_args()
    build = args.build.resolve()
    assert (build / '.mmu').read_text().strip() == '1'
    output = build / 'nanox'
    output.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='nanox-engine-') as directory:
        tmp = Path(directory)
        def image(name):
            path = tmp / name
            subprocess.run(['node', str(NX / 'tools/desktop-image.mjs'), str(build), str(path), '--engine'], check=True, capture_output=True)
            return path

        def run(disk, commands, *extra):
            result = subprocess.run([
                str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'),
                '--disk', str(disk), '--memory', '16384000', '--jit', '--headless',
                '--input', commands, '--timeout', '120', *extra,
            ], capture_output=True, text=True, timeout=130)
            assert result.returncode == 0, result.stdout + result.stderr
            assert 'FAILED' not in result.stdout and 'panic:' not in result.stdout, result.stdout + result.stderr
            return result.stdout

        disk = image('checks.img')
        log = run(disk, 'nxdraw --check\necho FIRST:$?\nnxdraw --check\necho REOPEN:$?\nshutdown\n')
        (output / 'engine-check.log').write_text(log)
        assert log.count('NANOX_ENGINE_OK') == 2 and 'FIRST:0' in log and 'REOPEN:0' in log, log
        subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(disk)], check=True)
        raw = output / 'engine.rgb565'
        report = output / 'frame-report.json'
        run(image('frame.img'), 'nxdraw --frame\n', '--raw-frame', str(raw), '--capture-frame', '1', '--report', str(report))
        expected = reference()
        data = raw.read_bytes()
        assert len(data) == WIDTH * HEIGHT * 2, len(data)
        actual = struct.unpack('<' + 'H' * len(expected), data)
        for i, (got, wanted) in enumerate(zip(actual, expected)):
            assert got == wanted, f'pixel ({i % WIDTH},{i // WIDTH}): {got:04x} != {wanted:04x}'
        # Inject directly through RVM's live event API after readiness. Riplix
        # uses its own timer device, not the legacy DEVICE presentation clock
        # used by --input-script / stepped execution.
        disk = image('events.img')
        events = subprocess.run(['cargo', 'test', '--release', '-p', 'rvm', '--test',
                                 'nanox_engine', '--', '--ignored', '--nocapture'],
                                cwd=ROOT, env=dict(os.environ, NANOX_TEST_BUILD=str(build), NANOX_EVENT_IMAGE=str(disk)),
                                capture_output=True, text=True, timeout=330)
        (output / 'event-check.log').write_text(events.stdout + events.stderr)
        assert events.returncode == 0, events.stdout + events.stderr
        subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(disk)], check=True)
    print('Nano-X JIT passed: C16 widths, regions/holes, clipping, dash bit 31, XOR, fonts, clipped bitmaps, offscreen and overlapping/clipped blits, Escape/Q/close events and display reuse; all 307200 RGB565 pixels match')


if __name__ == '__main__':
    main()
