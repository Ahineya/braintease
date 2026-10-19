#!/usr/bin/env python3
"""Directory rename invariants and Commander F6/r on disposable guest disks."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
sys.path.insert(0, str(ROOT / 'rfl/tools'))
from test_os import Guest
from test_symlinks import check_disk


def run(command):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)


def exercise(args, system, work):
    native = system == 'fuse-x'
    build = (args.native_build if native else args.build).resolve()
    if not args.skip_build:
        if native:
            run(['make', '-C', ROOT/'fuse-x', 'BUILD='+str(build), '-j4', build/'fuse-x'])
            run(['python3', ROOT/'fuse-x/cc.py', PORT/'tools/dirmovecheck.c', '--build', build,
                 '-o', build/'apps/dirmovecheck.fxe'])
        else:
            run(['make', '-C', PORT, 'BUILD='+str(build), '-j4', 'kernel', 'bios', build/'dirmovecheck.rxe'])
        # No application change is needed: exercise the existing Commander
        # against the fixed kernel, building it only when it is absent.
        commander = build/('apps/rfl-fm.fxe' if native else 'rfl-fm.rxe')
        if not commander.exists():
            run([ROOT/'target/release/rfl', 'build', ROOT/'rfl/apps/commander/main.rfl', '--os', system,
                 '--build-dir', build, '-o', commander])
    stage = work/'stage'; stage.mkdir()
    image = work/'root.img'
    names = ['sh','cat','echo','rfl-fm','dirmovecheck']
    if native:
        apps = stage/'apps'; apps.mkdir()
        for name in ['native-hello',*names]: (apps/(name+'.fxe')).symlink_to(build/'apps'/(name+'.fxe'))
        (stage/'application-report.json').write_text(json.dumps([dict(name=n,status='built') for n in names]))
        run(['node', ROOT/'fuse-x/image.mjs', 'create', image, '--apps', apps])
        command = [build/'fuse-x', '--run', image]
    else:
        for name in ['init',*names]: (stage/(name+'.rxe')).symlink_to(build/(name+'.rxe'))
        (stage/'FUZIX.SYS').symlink_to(build/'FUZIX.SYS')
        (stage/'apps.json').write_text(json.dumps(['init',*names,'mc']))
        run(['node', PORT/'tools/disk.mjs', stage, image])
        command = [ROOT/'target/release/rvm', build/'bios.bin', '--disk', image, '--memory','16384000',
                   '--jit', '--net','off', '--guest-ctrl-c', '--timeout','300']
    for boot in range(2):
        guest = Guest(command, system)
        try:
            guest.raw(guest.prompt)
            def prompt(title, value=''):
                def shown():
                    text=guest.screen.text()
                    return (title+':' in text or (title in text and 'Enter accept' in text)) and value in text
                guest.wait(shown, title+' dialog '+value)
            if boot:
                output = guest.shell('dirmovecheck persist; echo DIR_STATUS:$?', seconds=90)
                assert b'DIRMOVE_PERSIST_OK' in output and b'DIR_STATUS:0' in output, output
            else:
                output = guest.shell('dirmovecheck; echo DIR_STATUS:$?', seconds=120)
                assert b'DIRMOVE_OK' in output and b'DIR_STATUS:0' in output, output
                guest.send(b'mc /cmd-left /cmd-right\r')
                guest.visible('RFL Commander')
                guest.send(b'\x1b[B')
                guest.visible('Selected: /cmd-left/tree')
                guest.send(b'\x1b[17~')
                prompt('Move to','/cmd-right/tree')
                guest.send(b'\x1b')
                guest.wait(lambda: 'Move to:' not in guest.screen.text() and 'Enter accept' not in guest.screen.text(), 'directory move cancelled')
                guest.send(b'\x1b[17~')
                prompt('Move to','/cmd-right/tree')
                guest.send(b'\r')
                guest.visible('Moved successfully')
                guest.send(b'\t\x1b[B')
                guest.visible('Selected: /cmd-right/tree')
                guest.send(b'r')
                prompt('Rename','tree')
                guest.send(b'\x15renamed tree\r')
                guest.visible('Renamed successfully')
                guest.visible('Selected: /cmd-right/renamed tree')
                guest.send(b'\x1b[17~')
                prompt('Move to')
                guest.send(b'\x15/cmd-right/renamed tree/deep/loop\r')
                guest.visible('Invalid argument')
                guest.send(b'\x1b[17~')
                prompt('Move to')
                guest.send(b'\x15/cmd-left/tree\r')
                guest.visible('Moved successfully')
                guest.send(b'\x1b[21~')
                guest.raw(guest.prompt)
                output = guest.shell('dirmovecheck persist; echo DIR_STATUS:$?', seconds=90)
                assert b'DIRMOVE_PERSIST_OK' in output and b'DIR_STATUS:0' in output, output
            guest.send(b'shutdown\r')
            guest.process.wait(timeout=30)
            assert guest.process.returncode == 0
        finally:
            guest.close()
        check_disk(image, native)
    print(system+': directory identities, parents/link counts, collision/permission guards, Commander move/rename and reboot/fsck passed', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os', choices=('fuzix','fuse-x','both'), default='both')
    parser.add_argument('--build', type=Path, default=ROOT/'build/fuzix')
    parser.add_argument('--native-build', type=Path, default=ROOT/'fuse-x/build')
    parser.add_argument('--skip-build', action='store_true')
    args = parser.parse_args()
    for system in (['fuse-x','fuzix'] if args.os=='both' else [args.os]):
        with tempfile.TemporaryDirectory(prefix='directory-moves-') as directory:
            exercise(args,system,Path(directory))


if __name__=='__main__': main()
