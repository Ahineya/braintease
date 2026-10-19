#!/usr/bin/env python3
"""Measure Commander navigation and Rift source editing on disposable disks.

Use existing artifacts; run without concurrent builds for comparable timings.
These are end-to-end headless terminal measurements, including cold JIT/loading,
kernel I/O and ANSI decoding. They do not measure GUI compositor latency.
"""
import argparse
import json
import statistics
import tempfile
import time
from pathlib import Path

from bench_rift import cpu_clock
from test_os import Guest, ROOT
from test_rift import make_image


def summary(samples):
    return dict(median_ms=round(statistics.median(s[0] for s in samples), 2),
                median_cpu_ms=round(statistics.median(s[1] for s in samples), 2)
                if all(s[1] is not None for s in samples) else None,
                max_ms=round(max(s[0] for s in samples), 2),
                median_output_bytes=statistics.median(s[2] for s in samples),
                samples_ms=[round(s[0], 2) for s in samples])


def timed(guest, data, condition, description):
    before = len(guest.log)
    started = time.perf_counter()
    cpu_started = cpu_clock(guest.process.pid)
    guest.send(data)
    # Each condition checks changed content AND the final cursor position.
    guest.wait(condition, description, 120)
    cpu_finished = cpu_clock(guest.process.pid)
    return (1000 * (time.perf_counter() - started),
            1000 * (cpu_finished - cpu_started) if cpu_started is not None else None,
            len(guest.log) - before)


def frame(guest, rows):
    return guest.screen.y == rows - 2 and guest.screen.x == 0


def commander(guest, rows, columns, samples):
    def selected(name):
        path = '/tmp' if name == '..' else '/tmp/panel/' + name
        return ('Selected: ' + path == ''.join(guest.screen.lines[rows - 2]).strip()
                and frame(guest, rows))

    startup = timed(guest, b'rfl-fm /tmp/panel /tmp/panel\r',
                    lambda: selected('..') and '10 Quit' in guest.screen.text(), 'Commander first frame')
    results = dict(startup=summary([startup]))
    # Stay within one viewport, then force scrolling. Each pair returns to its
    # initial selection, making the samples independent of directory position.
    pairs = []
    for _ in range(samples):
        pairs.append(timed(guest, b'j', lambda: selected('item-000.txt'), 'Commander next row'))
        timed(guest, b'k', lambda: selected('..'), 'Commander previous row')
    results['selection'] = summary(pairs)
    guest.send(b'\x1b[F')
    guest.wait(lambda: selected('item-199.txt'), 'Commander last row')
    scrolling = []
    # Reset the viewport, then step beyond its bottom edge. Avoid timing a
    # clamped key that produces no physical output.
    guest.send(b'\x1b[H')
    guest.wait(lambda: selected('..'), 'Commander first row')
    visible = rows - 8
    for index in range(visible - 1):
        guest.send(b'j')
        guest.wait(lambda index=index: selected(f'item-{index:03d}.txt'), 'Commander bottom row')
    for index in range(samples):
        scrolling.append(timed(guest, b'j', lambda index=index: selected(f'item-{visible - 1 + index:03d}.txt'),
                               'Commander scroll'))
    results['scroll'] = summary(scrolling)
    guest.send(b'q')
    guest.raw(guest.prompt)
    print(json.dumps(dict(app='commander', system=guest.system, rows=rows, columns=columns, **results)), flush=True)


def rift(guest, rows, columns, samples):
    startup = timed(guest, b'rift /tmp/full.rfl\r',
                    lambda: 'F10 Menu' in guest.screen.text() and 'NORMAL' in guest.screen.text()
                    and guest.screen.y == 2 and guest.screen.x == 7, 'Rift first source frame')
    results = dict(startup=summary([startup]))
    motions = []
    for _ in range(samples):
        motions.append(timed(guest, b'l', lambda: guest.screen.y == 2 and guest.screen.x == 8,
                             'Rift cursor right'))
        timed(guest, b'h', lambda: guest.screen.y == 2 and guest.screen.x == 7, 'Rift cursor left')
    results['cursor'] = summary(motions)
    guest.send(b'i')
    guest.wait(lambda: 'INSERT' in guest.screen.text(), 'Rift insert mode')
    typed = []
    text = 'abcdefghijklmnopqrstuvwx'[:samples]
    for index, character in enumerate(text):
        typed.append(timed(guest, character.encode(),
                           lambda index=index: ''.join(guest.screen.lines[2][7:8 + index]) == text[:index + 1]
                           and guest.screen.y == 2 and guest.screen.x == 8 + index, 'Rift source edit'))
    results['typing'] = summary(typed)
    guest.send(b'\x1b')
    guest.visible('NORMAL')
    guest.send(b':')
    guest.visible(': command')
    guest.send(b'qa!\r')
    guest.raw(guest.prompt)
    print(json.dumps(dict(app='rift', system=guest.system, rows=rows, columns=columns,
                          document_lines=200, **results)), flush=True)


def measure(system, args):
    build = ROOT / ('build/fuzix' if system == 'fuzix' else 'fuse-x/build')
    extension = 'rxe' if system == 'fuzix' else 'fxe'
    artifacts = args.artifacts / system if args.artifacts else build / ('' if system == 'fuzix' else 'apps')
    fixtures = {f'/tmp/panel/item-{i:03d}.txt': 'file\n' for i in range(200)}
    fixtures['/tmp/full.rfl'] = 'fn main() {\n' + ''.join(
        f'    let value_{i} = call(123, "example"); // highlighted source\n' for i in range(198)) + '}\n'
    with tempfile.TemporaryDirectory(prefix='tui-bench-', dir='/tmp') as directory:
        image = make_image(system, build, (artifacts / ('rift.' + extension)).resolve(), Path(directory),
                           extra_files=[dict(name='/bin/rfl-fm', source=str((artifacts / ('rfl-fm.' + extension)).resolve()))],
                           extra_fixtures=fixtures)
        command = [build / 'fuse-x', '--run', image] if system == 'fuse-x' else [
            args.rvm, build / 'bios.bin', '--disk', image, '--memory', '16384000',
            '--net', 'off', '--jit', '--guest-ctrl-c', '--timeout', '600']
        guest = Guest(command, system)
        try:
            guest.raw(guest.prompt)
            for rows, columns in [(24, 80), (40, 120)]:
                guest.resize(rows, columns)
                guest.pump()
                commander(guest, rows, columns, args.samples)
                rift(guest, rows, columns, args.samples)
            guest.send(b'shutdown\r')
            guest.process.wait(timeout=30)
            assert guest.process.returncode == 0
        finally:
            guest.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os', choices=['fuzix', 'fuse-x', 'both'], default='both')
    parser.add_argument('--artifacts', type=Path, help='directory with fuzix/ and fuse-x/ app artifacts')
    parser.add_argument('--samples', type=int, choices=range(1, 25), default=12)
    parser.add_argument('--rvm', type=Path, default=ROOT / 'target/release/rvm')
    args = parser.parse_args()
    for system in (['fuzix', 'fuse-x'] if args.os == 'both' else [args.os]):
        measure(system, args)


if __name__ == '__main__':
    main()
