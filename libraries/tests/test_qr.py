#!/usr/bin/env python3
"""Check the C16 QR engine and scan actual 80x30 guest terminal output."""
import argparse
import fcntl
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest

SAMPLE = 'https://example.org/fuse-x/qr-demo?sample=0123456789abcdef01'


def run(command, **kwargs):
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def matrix(text):
    match = re.search(r'QR_BEGIN\n([.UL#\n]+)QR_END', text.replace('\r', ''))
    assert match, text
    rows = match[1].strip().splitlines()
    assert len({len(row) for row in rows}) == 1
    assert len(rows) <= 23 and len(rows[0]) <= 45
    return rows


def screen(helper, stream):
    text = subprocess.check_output([str(helper), '--screen'], input=stream).decode()
    return [tuple(map(int, line.split(':'))) for line in text.splitlines()]


def check_display(cells, expected):
    width, height = len(expected[0]), len(expected)
    top, left = 3 + (30 - 5 - height) // 2, (80 - width) // 2
    codes = {'.': 32, 'U': 0x2580, 'L': 0x2584, '#': 0x2588}
    for y, row in enumerate(expected):
        for x, value in enumerate(row):
            actual = cells[(top + y) * 80 + left + x]
            assert actual == (codes[value], 0xf0), (x, y, value, actual)
    assert any(ch in (0x2588, 0x2584, 0x2580) for ch, _ in cells)


def write_png(path, cells):
    # Render the actual VT cells with the same ROM 8x16 font as nxterm.
    source = (ROOT / 'ports/nanox/upstream/src/fonts/compiled/rom8x16.c').read_text(errors='replace')
    body = source.split('rom8x16_bits[] = {', 1)[1].split('};', 1)[0]
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    font = [int(value, 16) >> 8 for value in re.findall(r'0x[0-9a-fA-F]+', body)]
    assert len(font) == 256 * 16
    palette = [(0, 0, 0), (170, 0, 0), (0, 170, 0), (170, 85, 0),
               (0, 0, 170), (170, 0, 170), (0, 170, 170), (170, 170, 170),
               (85, 85, 85), (255, 85, 85), (85, 255, 85), (255, 255, 85),
               (85, 85, 255), (255, 85, 255), (85, 255, 255), (255, 255, 255)]
    pixels = bytearray()
    for y in range(480):
        pixels.append(0)
        for x in range(640):
            ch, colour = cells[(y // 16) * 80 + x // 8]
            ink = font[{0x2580:223,0x2584:220,0x2588:219}.get(ch,ch if ch<256 else 63) * 16 + y % 16] & (128 >> (x % 8))
            pixels.extend(palette[(colour & 15) if ink else (colour >> 4) & 15])
    def chunk(kind, data):
        return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!IIBBBBB', 640, 480, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('host', 'native', 'ripple', 'both'), default='native')
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--skip-build', action='store_true')
    parser.add_argument('--no-scan', action='store_true', help='skip macOS scanner when its system services are sandboxed')
    args = parser.parse_args()
    build = ROOT / 'build/qr-test'
    build.mkdir(parents=True, exist_ok=True)
    helper = build / 'qr-check'
    flags = ['-std=c99', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined']
    if sys.platform == 'darwin':
        flags += ['-isysroot', os.environ.get('SDKROOT') or subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()]
    run([*shlex.split(args.cc), *flags, ROOT / 'libraries/tests/qr.c', ROOT / 'libraries/qr/qr.c',
         ROOT / 'ports/graphics/terminal.c', ROOT / 'libraries/unicode/unicode.c', '-o', helper])
    run([helper])
    targets = ('native', 'ripple') if args.target == 'both' else (() if args.target == 'host' else (args.target,))
    scanner = None
    if targets and not args.no_scan and sys.platform == 'darwin' and shutil.which('swiftc'):
        scanner = build / 'decode-qr'
        run(['swiftc', '-module-cache-path', build / 'swift-cache', ROOT / 'libraries/tests/decode_qr.swift', '-o', scanner])
    for target in targets:
        artifact = ROOT / ('fuse-x/build/apps/qr-demo.fxe' if target == 'native' else 'build/fuzix/qr-demo.rxe')
        if not args.skip_build:
            run([sys.executable, ROOT / 'rfl/tools/build_os.py', ROOT / 'rfl/examples/qr-demo.rfl',
                 '--os', 'fuse-x' if target == 'native' else 'fuzix', '-o', artifact])
        with tempfile.TemporaryDirectory(prefix='qr-guest-', dir='/tmp') as temporary:
            guest = Guest(temporary, target, {'qr-demo': artifact}, files=[dict(name='/etc/termcap',
                text='ansi|vt100|xterm:co#80:li#30:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:me=\\E[0m:ce=\\E[K:\n')])
            active = False
            try:
                fcntl.ioctl(guest.master, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 80, 0, 0))
                expected = matrix(subprocess.check_output([str(helper), '--dump', SAMPLE], text=True))
                for payload in (SAMPLE, 'x' * 84):
                    reference = matrix(subprocess.check_output([str(helper), '--dump', payload], text=True))
                    output = guest.shell(f'qr-demo --dump {payload}; echo QR_STATUS:$?')
                    assert matrix(output) == reference and 'QR_STATUS:0' in output, output
                output = guest.shell('qr-demo --dump ' + 'x' * 85 + '; echo QR_STATUS:$?')
                assert '84-byte display limit' in output and 'QR_STATUS:1' in output, output
                at = len(guest.log)
                os.write(guest.master, b'qr-demo; echo QR_STATUS:$?\r')
                active = True
                guest.read(b'\x1b[30;21H', 30)
                rendered = bytes(guest.log[at:])
                (build / f'qr-{target}.ansi').write_bytes(rendered)
                cells = screen(helper, rendered)
                check_display(cells, expected)
                image = build / f'qr-{target}.png'
                write_png(image, cells)
                if scanner:
                    run([scanner, image, SAMPLE])
                # Shrinking must remove the whole symbol instead of clipping it.
                guest.at = len(guest.log)
                fcntl.ioctl(guest.master, termios.TIOCSWINSZ, struct.pack('HHHH', 20, 40, 0, 0))
                output = guest.read(b'\x1b[20;21H', 30)
                assert 'Screen too small' in output
                small = screen(helper, bytes(guest.log[at:]))
                assert not any(ch in (0x2588, 0x2584, 0x2580) for ch, _ in small)
                guest.at = len(guest.log)
                fcntl.ioctl(guest.master, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 80, 0, 0))
                guest.read(b'\x1b[30;21H', 30)
                check_display(screen(helper, bytes(guest.log[at:])), expected)
                os.write(guest.master, b'q')
                output = guest.read(b'QR_STATUS:0')
                assert '\x1b[?25h' in output and '\x1b[?1049l' in output, output
                guest.read(guest.prompt)
                active = False
                print(f'{target}: matching C16 QR, 80x30 display, resize/refusal and terminal restoration passed; {image}', flush=True)
            finally:
                # Also leave a still-running TUI after a failed assertion.
                if active:
                    os.write(guest.master, b'q')
                    guest.pump()
                guest.close()


if __name__ == '__main__':
    main()
