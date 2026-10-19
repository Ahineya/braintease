#!/usr/bin/env python3
"""Exercise real Rift UTF-8 input, editing and saved octets on both kernels."""
import argparse
import codecs
import importlib.util
import mmap
import struct
import tempfile
import time
import unicodedata
from pathlib import Path
from test_os import Guest, Screen, ROOT, PORT, run
from test_rift import make_image


class UnicodeScreen(Screen):
    """Independent UTF-8 sink for these Latin/Cyrillic/CJK/ZWJ fixtures.

    Reuse ANSI handling; Python Unicode properties supply cell widths. The
    final cursor CSI and saved disk octets remain independent assertions.
    """
    def __init__(self):
        super().__init__()
        self.decoder = codecs.getincrementaldecoder('utf-8')('replace')
        self.last = None
        self.join = False

    def feed(self, data):
        for ch in self.decoder.decode(data):
            if ord(ch) < 128:
                text = ch >= ' ' and not self.pending
                y = min(self.rows - 1, self.y + int(self.wrap))
                x = 0 if self.wrap else self.x
                super().feed(ch.encode())
                self.last = (y, x) if text else None
                self.join = False
            elif (unicodedata.combining(ch) or ch in ('\u200d', '\ufe0f') or self.join) and self.last:
                y, x = self.last
                self.lines[y][x] += ch
                self.join = ch == '\u200d'
            else:
                y = min(self.rows - 1, self.y + int(self.wrap))
                x = 0 if self.wrap else self.x
                width = 2 if unicodedata.east_asian_width(ch) in ('W', 'F') else 1
                super().feed(b'\x80' * width)
                if x < self.columns:
                    self.lines[y][x] = ch
                    if width == 2 and x + 1 < self.columns:
                        self.lines[y][x + 1] = ''
                self.last, self.join = (y, x), False


def exercise(args, system, directory):
    native = system == 'fuse-x'
    build = (args.native_build if native else args.fuzix_build).resolve()
    name = 'rift.' + ('fxe' if native else 'rxe')
    artifact = args.prebuilt.resolve() / system / name if args.prebuilt else directory / name
    if not args.prebuilt:
        run([args.rfl.resolve(), 'build', ROOT / 'rfl/apps/rift/main.rfl', '--os', system,
             '--build-dir', build, '-o', artifact])
    assert artifact.is_file(), artifact
    initial = 'Жé中👩‍💻Ă\n'
    disk = make_image(system, build, artifact, directory,
                      extra_fixtures={'/tmp/unicode.txt': initial})
    command = [build / 'fuse-x', '--run', disk] if native else [
        ROOT / 'target/release/rvm', build / 'bios.bin', '--disk', disk,
        '--memory', '16384000', '--net', 'off', '--jit', '--guest-ctrl-c', '--timeout', '600']
    guest = Guest(command, system)
    guest.screen = UnicodeScreen()

    def cursor(column):
        guest.wait(lambda: guest.screen.y == 2 and guest.screen.x == 7 + column,
                   'Unicode cursor display column ' + str(column))

    def colon(text):
        guest.send(b':'); guest.visible(': command'); guest.send(text.encode() + b'\r')

    def saved(path):
        guest.visible('Saved ' + path)

    try:
        guest.raw(guest.prompt)
        guest.send(b'rift /tmp/unicode.txt\r')
        guest.visible('NORMAL'); guest.raw(initial.rstrip('\n').encode())
        guest.send(b'0llx\x13'); saved('/tmp/unicode.txt')
        colon('qa'); guest.raw(guest.prompt)
        path, copied = '/tmp/юникод.txt', '/tmp/副本.txt'
        guest.send(b'rift /tmp/created.txt\r'); guest.visible('NORMAL')
        guest.send(b'i'); guest.visible('INSERT')
        # Split every UTF-8 sequence across reads. U+0102 must remain text,
        # never the numerically identical curses Down key (258).
        for byte in 'Жé中👩‍💻Ă!'.encode():
            guest.send(bytes([byte])); time.sleep(.005)
        cursor(8)
        guest.send(b'\x7f\x7f\x7f'); cursor(4)  # !, Ă, whole emoji ZWJ
        guest.send(b'Q\x1b[D\x7f'); cursor(2)  # delete 中 before Q
        guest.send(b'\x1blxu.'); guest.visible('NORMAL')
        guest.send(b'A'); guest.visible('INSERT')
        guest.send('Ă!'.encode()); cursor(4)
        guest.send(b'\x1b\x13'); saved('/tmp/created.txt')
        colon('w ' + path); saved(path)
        colon('w ' + copied); saved(copied)
        colon('qa'); guest.raw(guest.prompt)
        guest.send(b'rift\r'); guest.visible('NORMAL')
        guest.send(b'\x1bOR'); guest.visible('Open file')
        guest.send(path.encode() + b'\r'); guest.visible('Opened ' + path)
        colon('qa'); guest.raw(guest.prompt)
        assert not guest.screen.alternate
        guest.send(b'shutdown\r'); guest.process.wait(timeout=30)
        assert guest.process.returncode == 0
        spec = importlib.util.spec_from_file_location('rift_unicode_reader', PORT / 'tools/root_reader.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        if native:
            class Reader(module.RootReader):
                def __init__(self, image):
                    with open(image, 'rb') as file:
                        self.disk = mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ)
                def word(self, cell):
                    return struct.unpack_from('<H', self.disk, cell * 2)[0]
        else:
            Reader = module.RootReader
        reader = Reader(disk)
        try:
            assert bytes(reader.read('/tmp/unicode.txt')) == 'Жé👩‍💻Ă\n'.encode()
            for file in (path, copied):
                # RootReader exposes directory names as raw octet cells.
                actual = bytes(reader.read(file.encode().decode('latin1')))
                assert actual == 'ЖéĂ!'.encode(), (file, actual)
        finally:
            reader.disk.close()
        print(system + ': Unicode display/input, split UTF-8, whole-grapheme editing, '
              'cursor columns, undo/dot, Unicode paths and saved bytes passed', flush=True)
    finally:
        Path('/tmp/rift-unicode-' + system + '.log').write_bytes(guest.log)
        guest.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os', choices=['both', 'fuse-x', 'fuzix'], default='both')
    parser.add_argument('--rfl', type=Path, default=ROOT / 'target/release/rfl')
    parser.add_argument('--native-build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--fuzix-build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--prebuilt', type=Path, help='explicitly reuse previously built test artifacts')
    args = parser.parse_args()
    for system in (['fuse-x', 'fuzix'] if args.os == 'both' else [args.os]):
        with tempfile.TemporaryDirectory(prefix='rift-unicode-' + system + '-') as directory:
            exercise(args, system, Path(directory))


if __name__ == '__main__':
    main()
