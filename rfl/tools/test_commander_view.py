#!/usr/bin/env python3
"""Exercise Commander's wrapped Unicode preview on both real guest kernels."""
import argparse
from pathlib import Path
import tempfile
from test_os import Guest, ROOT
from test_rift import make_image
from test_rift_unicode import UnicodeScreen
from test_rift_paste import reader


def exercise(system, directory):
    native = system == 'fuse-x'
    build = ROOT / ('fuse-x/build' if native else 'build/fuzix')
    artifact = build / ('apps/rfl-fm.fxe' if native else 'rfl-fm.rxe')
    rift = build / ('apps/rift.fxe' if native else 'rift.rxe')
    original = ('START ' + ' '.join(f'word{i:03d}' for i in range(250)) + ' LONG_TAIL\r\n'
                + 'SECOND_LINE\r\n' + 'Жé 中 👩‍💻 ' * 24 + 'UNICODE_TAIL\r\n'
                + '\tTAB_CELL\r\n\r\nEND_FILE\r\n')
    disk = make_image(system, build, rift, directory,
                      extra_files=[dict(name='/bin/rfl-fm', source=str(artifact))],
                      extra_fixtures={'/tmp/view/long.txt': original})
    command = [build / 'fuse-x', '--run', disk] if native else [
        ROOT / 'target/release/rvm', build / 'bios.bin', '--disk', disk, '--memory', '16384000',
        '--net', 'off', '--jit', '--guest-ctrl-c', '--timeout', '600']
    guest = Guest(command, system)
    guest.screen = UnicodeScreen()
    def first():
        return ''.join(guest.screen.lines[3])
    try:
        guest.raw(guest.prompt)
        guest.send(b'rfl-fm /tmp/view /tmp/view\r'); guest.visible('RFL Commander')
        guest.visible('Selected: /tmp')
        guest.send(b'j'); guest.visible('Selected: /tmp/view/long.txt')
        guest.send(b'v'); guest.visible('View: /tmp/view/long.txt'); guest.visible('Wrap:on [w]')
        guest.wait(lambda: 'START word000' in first() and 'word008' in ''.join(guest.screen.lines[4]),
                   'long logical line wraps into the next display row')
        guest.send(b'\x1b[B'); guest.wait(lambda: 'word008' in first(), 'Down scrolls one visual row')
        guest.send(b'\x1b[A'); guest.wait(lambda: 'START word000' in first(), 'Up restores first visual row')
        guest.send(b'\x1b[6~'); guest.wait(lambda: 'START word000' not in first(), 'Page Down scrolls wrapped text')
        guest.send(b'\x1b[F'); guest.visible('END_FILE'); guest.visible('UNICODE_TAIL'); guest.visible('LONG_TAIL')
        assert 'Жé' in guest.screen.text() and '👩‍💻' in guest.screen.text(), guest.screen.text()
        guest.send(b'\x1b[H'); guest.wait(lambda: 'START word000' in first(), 'Home restores the start')
        guest.send(b'w'); guest.visible('Wrap:off [w]'); guest.wait(lambda: 'SECOND_LINE' in ''.join(guest.screen.lines[4]), 'nowrap retains logical rows')
        for _ in range(8): guest.send(b'\x1b[C')
        guest.wait(lambda: 'START word000' not in first() and 'word004' in first(), 'Right scrolls long logical lines')
        for _ in range(8): guest.send(b'\x1b[D')
        guest.wait(lambda: 'START word000' in first(), 'Left restores horizontal origin')
        guest.send(b'w'); guest.visible('Wrap:on [w]')
        guest.resize(30, 60)
        # Viewer is centered at row 2; its footer moves to row 26 after resize.
        guest.wait(lambda: 'Wrap:on [w]' in ''.join(guest.screen.lines[26]), 'complete resized viewer')
        guest.wait(lambda: 'START word000' in first() and 'word006' in ''.join(guest.screen.lines[4]), 'long line rewraps to narrower window')
        guest.send(b'\x1b[F'); guest.visible('END_FILE'); guest.visible('UNICODE_TAIL')
        guest.send(b'q'); guest.wait(lambda: 'View: /tmp/view/long.txt' not in guest.screen.text(), 'viewer closes cleanly')
        guest.visible('Selected: /tmp/view/long.txt')
        guest.send(b'v'); guest.visible('Wrap:on [w]'); guest.send(b'\x1b')
        guest.wait(lambda: 'View: /tmp/view/long.txt' not in guest.screen.text(), 'Escape closes reopened preview')
        guest.send(b'q'); guest.raw(guest.prompt)
        guest.send(b'shutdown\r'); guest.process.wait(timeout=30)
        assert guest.process.returncode == 0
        disk_reader = reader(system, disk)
        try: assert bytes(disk_reader.read('/tmp/view/long.txt')) == original.encode(), 'Viewer modified the file'
        finally: disk_reader.disk.close()
        print(system + ': wrapped rows, paging/Home/End, Unicode/CRLF/tabs, wrap toggle, horizontal scrolling, resize and unchanged file passed', flush=True)
    finally:
        Path('/tmp/commander-view-' + system + '.log').write_bytes(guest.log)
        guest.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os', choices=['both', 'fuse-x', 'fuzix'], default='both')
    args = parser.parse_args()
    for system in (['fuse-x', 'fuzix'] if args.os == 'both' else [args.os]):
        with tempfile.TemporaryDirectory(prefix='commander-view-') as directory:
            exercise(system, Path(directory))

if __name__ == '__main__': main()
