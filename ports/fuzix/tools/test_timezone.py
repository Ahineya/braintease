#!/usr/bin/env python3
"""Check civil time and an idle mux clock in disposable native/RVM guests."""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest
from test_mux_restore import Terminal


def probe(target, work):
    native = target == 'native'
    build = ROOT / ('fuse-x/build' if native else 'build/fuzix')
    output = work / ('tzcheck.fxe' if native else 'tzcheck.rxe')
    if native:
        subprocess.run([sys.executable, str(ROOT / 'fuse-x/cc.py'),
                        str(PORT / 'tools/timezonecheck.c'), '-o', str(output)], check=True)
    else:
        includes = [PORT / 'ripple/include', PORT / 'upstream/Library/include',
                    ROOT / 'runtime/include', PORT / 'ripple']
        assembly, obj = work / 'tzcheck.asm', work / 'tzcheck.pobj'
        subprocess.run([str(ROOT / 'target/release/rcc'), 'compile',
                        str(PORT / 'tools/timezonecheck.c'), '-o', str(assembly),
                        '--bank-size', '64000', '-DRIPPLE_MMU=1', '-DNO_64BIT',
                        *[f'-I{p}' for p in includes]], check=True)
        subprocess.run([str(ROOT / 'src/ripple-asm/target/release/rasm'), 'assemble',
                        str(assembly), '-o', str(obj), '--bank-size', '64000',
                        '--max-immediate', '65535'], check=True)
        libraries = subprocess.check_output([
            'make', '-s', '--no-print-directory', '-C', str(PORT), f'BUILD={build}',
            '-f', 'Makefile', '-f', '-', 'timezone-libs'],
            input='timezone-libs:\n\t@echo $(LIBC_OBJS)\n', text=True).split()
        subprocess.run([str(ROOT / 'src/ripple-asm/target/release/rlink'),
                        str(build / 'user/entry.pobj'), str(obj),
                        str(build / 'user/userlib.pobj'), *libraries,
                        '-f', 'rxe', '--bank-size', '64000', '-o', str(output)], check=True)
    return output


def run(target):
    native = target == 'native'
    build = ROOT / ('fuse-x/build' if native else 'build/fuzix')
    rule = 'CET-1CEST,M3.5.0/2,M10.5.0/3'
    with tempfile.TemporaryDirectory(prefix='mux-clock-', dir='/tmp') as directory:
        work = Path(directory)
        apps = {name: build / (f'apps/{name}.fxe' if native else f'{name}.rxe')
                for name in ('mux', 'date')}
        apps['tzcheck'] = probe(target, work)
        files = [dict(name='/etc/TZ', text=rule + '\n')]
        for i in range(16):
            files += [dict(name=f'/dev/ptyp{i:x}', mode=0o20600, device=0x300 + i),
                      dict(name=f'/dev/ttyp{i:x}', mode=0o20600, device=0x202 + i)]
        guest = Guest(work, target, apps, files, blocks=24576, disk_sectors=131072)
        term = Terminal(guest)
        try:
            result = guest.shell('tzcheck')
            assert 'TIMEZONE_OK 60 vectors' in result, result
            print(target + ': timezone vectors passed (DST boundaries, hemispheres, leap years, offsets, epoch limits, invalid settings)', flush=True)
            result = guest.shell('tzcheck --now')
            match = re.search(r'LOCAL_CLOCK (\d\d:\d\d) (CEST|CET)', result)
            assert match, result
            zone = match[2]
            result = guest.shell('date')
            assert zone in result, result
            result = guest.shell('date -u')
            assert 'UTC' in result, result
            result = guest.shell("sh -c 'TZ=NST3:30; export TZ; date'")
            assert 'NST' in result, result
            result = guest.shell('date')
            assert zone in result, result
            term.line('mux'); term.active = True; term.shown('1:shell 1')
            term.shown(' ' + match[1] + ' ')
            # Tabs never paint over the reserved clock cells, even long names.
            term.key(b','); term.shown('Rename tab:')
            term.send(b'\x15abcdefghijklmnopqrstuvwx\r'); term.shown('1:abcdefghijklmnopqrstuvwx')
            for _ in range(2):
                term.key(b'c'); term.shown('shell ' + str(_ + 2))
                term.key(b','); term.shown('Rename tab:')
                term.send(b'\x15abcdefghijklmnopqrstuvwx\r')
                term.shown(str(_ + 2) + ':abcdefghijklmnopqrstuvwx')
            clock = term.screen.text().splitlines()[0][-7:]
            assert re.fullmatch(r' \d\d:\d\d ', clock), repr(clock)
            # No keys or guest program output until the minute changes.
            end = time.monotonic() + 75
            while time.monotonic() < end:
                guest.pump()
                term.screen.feed(bytes(guest.log[term.at:])); term.at = len(guest.log)
                updated = term.screen.text().splitlines()[0][-7:]
                if updated != clock and re.fullmatch(r' \d\d:\d\d ', updated): break
            else: raise AssertionError('Idle clock did not advance: ' + repr(clock))
            print(target + ': /etc/TZ, environment override, date -u, tab overflow and idle mux clock passed', flush=True)
            term.leave()
        finally:
            if term.active: term.leave()
            guest.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('native', 'ripple'), default='native')
    run(parser.parse_args().target)
