#!/usr/bin/env python3
"""Real Rift soft wrapping, bulk paste, hard-format settings and persistence."""
import argparse
import mmap
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
from test_os import Guest, ROOT, PORT
from test_rift import make_image
from test_rift_unicode import UnicodeScreen


def reader(system, disk):
    sys.path.insert(0, str(PORT / 'tools'))
    from root_reader import RootReader
    if system == 'fuzix':
        return RootReader(disk)
    class Raw(RootReader):
        def __init__(self, path):
            with open(path, 'rb') as stream:
                self.disk = mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ)
        def word(self, cell):
            return struct.unpack_from('<H', self.disk, cell * 2)[0]
    return Raw(disk)


def exercise(system, artifact, directory, benchmark_only=False):
    native = system == 'fuse-x'
    build = ROOT / ('fuse-x/build' if native else 'build/fuzix')
    initial = 'alpha ' * 30 + 'omega\n'
    disk = make_image(system, build, artifact, directory, extra_fixtures={'/tmp/wrap.txt': initial})
    command = [build / 'fuse-x', '--run', disk] if native else [
        ROOT / 'target/release/rvm', build / 'bios.bin', '--disk', disk,
        '--memory', '16384000', '--net', 'off', '--jit', '--guest-ctrl-c', '--timeout', '600']
    guest = Guest(command, system)
    guest.screen = UnicodeScreen()

    def send(data):
        at = 0
        while at < len(data):
            at += os.write(guest.master, data[at:at + 4096])

    def colon(text):
        send(b':'); guest.visible(': command'); send(text.encode() + b'\r')

    def save(path):
        send(b'\x13'); guest.visible('Saved ' + path)

    try:
        guest.raw(guest.prompt)
        send(b'rift /tmp/bench.txt\r'); guest.visible('NORMAL')
        send(b'i'); guest.visible('INSERT')
        payload = b'x' * 512
        start = time.monotonic(); send(payload)
        guest.wait(lambda: '1:513' in guest.screen.text(), '512-octet input burst', 120)
        elapsed = time.monotonic() - start
        print(f'{system}: 512-octet unbracketed burst {elapsed:.3f}s ({artifact})', flush=True)
        send(b'\x1b'); guest.visible('NORMAL'); colon('qa!'); guest.raw(guest.prompt)
        if benchmark_only:
            send(b'shutdown\r'); guest.process.wait(timeout=30)
            return elapsed

        send(b'rift /tmp/wrap.txt\r'); guest.visible('NORMAL')
        guest.wait(lambda: 'alpha' in ''.join(guest.screen.lines[3]), 'soft-wrapped continuation')
        assert ''.join(guest.screen.lines[2])[7:].startswith('alpha')
        send(b'$'); guest.wait(lambda: guest.screen.y > 2, 'cursor on a continuation row')
        send(b'0'); guest.wait(lambda: guest.screen.y == 2 and guest.screen.x == 7, 'logical Home')
        send(b'\x1b[B'); guest.wait(lambda: guest.screen.y == 3 and guest.screen.x == 7, 'visual Down')
        send(b'\x1b[A'); guest.wait(lambda: guest.screen.y == 2 and guest.screen.x == 7, 'visual Up')
        guest.resize(30, 60)
        guest.wait(lambda: 'F1 Help' in ''.join(guest.screen.lines[29]), 'complete resized frame')
        guest.wait(lambda: 'alpha' in ''.join(guest.screen.lines[3]), 'rewrap after resize')
        save('/tmp/wrap.txt')
        colon('set nowrap'); guest.visible('Settings saved')
        send(b'$'); guest.wait(lambda: guest.screen.y == 2, 'horizontal mode remains configurable')
        colon('set wrap'); guest.visible('Settings saved')
        colon('e /tmp/pasted.txt'); guest.visible('pasted.txt')
        send(b'i'); guest.visible('INSERT')
        text = ('Український текст é 中 👩‍💻: слова для перевірки вставлення. ' * 70
                + '\r\nLiteral controls: \x1b:q! \x13\r\nPASTE_DONE_END')
        before = len(guest.log); start = time.monotonic()
        send(b'\x1b[200~' + text.encode())
        # End marker deliberately spans terminal reads.
        send(b'\x1b[20'); time.sleep(.01); send(b'1~')
        guest.wait(lambda: 'PASTE_DONE_END' in guest.screen.text(), 'complete Unicode paste', 180)
        paste_seconds = time.monotonic() - start
        print(f'{system}: {len(text.encode())}-octet Unicode paste {paste_seconds:.3f}s', flush=True)
        assert len(guest.log) - before < 100000, 'Paste still redraws per character'
        save('/tmp/pasted.txt')
        send(b'\x1b'); guest.visible('NORMAL')
        send(b'u')
        guest.wait(lambda: not ''.join(guest.screen.lines[2][7:]).strip()
                   and '1:1' in guest.screen.text(), 'one undo removes the entire paste')
        save('/tmp/pasted.txt')
        send(b'\x12'); save('/tmp/pasted.txt')
        colon('set textwidth=20'); guest.visible('Settings saved')
        colon('set formatoptions+=t'); guest.visible('Settings saved')
        colon('e /tmp/formatted.txt'); guest.visible('formatted.txt')
        send(b'i'); guest.visible('INSERT')
        words = b'one two three four five six seven eight nine'
        send(words); guest.wait(lambda: '3:5' in guest.screen.text(), 'hard wrapping while typing')
        save('/tmp/formatted.txt')
        send(b'\x1b'); guest.visible('NORMAL')
        colon('set formatoptions-=t'); guest.visible('Settings saved')
        colon('qa'); guest.raw(guest.prompt)
        send(b'rift /tmp/unformatted.txt\r'); guest.visible('NORMAL')
        send(b'i'); guest.visible('INSERT'); send(words)
        guest.wait(lambda: '1:45' in guest.screen.text(), 'formatoptions disable restored')
        save('/tmp/unformatted.txt'); send(b'\x1b'); guest.visible('NORMAL')
        colon('e /tmp/dot.txt'); guest.visible('dot.txt')
        send(b'i'); guest.visible('INSERT'); send(b'abc')
        guest.wait(lambda: '1:4' in guest.screen.text(), 'short typing burst')
        send(b'\x1b0.'); guest.wait(lambda: 'abcabc' in guest.screen.text(), 'burst dot replay')
        save('/tmp/dot.txt')
        colon('set formatoptions+=t'); guest.visible('Settings saved')
        colon('qa'); guest.raw(guest.prompt)
        send(b'rift /tmp/persisted.txt\r'); guest.visible('NORMAL')
        send(b'i'); guest.visible('INSERT'); send(words)
        guest.wait(lambda: '3:5' in guest.screen.text(), 'hard-format settings restored')
        save('/tmp/persisted.txt')
        send(b'\x1b'); guest.visible('NORMAL'); colon('qa'); guest.raw(guest.prompt)
        assert b'\x1b[?2004l' in guest.log, 'Bracketed paste mode was not restored'
        send(b'shutdown\r'); guest.process.wait(timeout=30)
        assert guest.process.returncode == 0
        disk_reader = reader(system, disk)
        try:
            assert bytes(disk_reader.read('/tmp/wrap.txt')) == initial.encode(), 'Soft wrap changed the file'
            assert bytes(disk_reader.read('/tmp/pasted.txt')) == text.encode(), 'Paste/undo/redo changed octets'
            expected = b'one two three four\nfive six seven eight\nnine'
            assert bytes(disk_reader.read('/tmp/formatted.txt')) == expected
            assert bytes(disk_reader.read('/tmp/unformatted.txt')) == words
            assert bytes(disk_reader.read('/tmp/dot.txt')) == b'abcabc'
            assert bytes(disk_reader.read('/tmp/persisted.txt')) == expected
            config = bytes(disk_reader.read('/.riftrc'))
            assert b'textwidth=20\n' in config and b'formatoptions=t\n' in config
        finally:
            disk_reader.disk.close()
        print(f'{system}: wrapping, Unicode paste ({len(text.encode())} octets, {paste_seconds:.3f}s), '
              'single undo/redo, literal controls, typing format and dotfile restart passed', flush=True)
        return elapsed
    finally:
        Path('/tmp/rift-paste-' + system + '.log').write_bytes(guest.log)
        guest.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os', choices=['both', 'fuse-x', 'fuzix'], default='both')
    parser.add_argument('--baseline', type=Path, help='directory with fuse-x/rift.fxe and fuzix/rift.rxe')
    args = parser.parse_args()
    for system in (['fuse-x', 'fuzix'] if args.os == 'both' else [args.os]):
        extension = 'fxe' if system == 'fuse-x' else 'rxe'
        artifact = ROOT / ('fuse-x/build/apps/rift.fxe' if system == 'fuse-x' else 'build/fuzix/rift.rxe')
        before = None
        if args.baseline:
            with tempfile.TemporaryDirectory(prefix='rift-paste-before-') as temporary:
                before = exercise(system, args.baseline.resolve() / system / ('rift.' + extension), Path(temporary), True)
        with tempfile.TemporaryDirectory(prefix='rift-paste-') as temporary:
            after = exercise(system, artifact, Path(temporary))
        if before:
            print(f'{system}: input-burst speedup {before / after:.1f}x', flush=True)


if __name__ == '__main__':
    main()
