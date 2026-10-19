#!/usr/bin/env python3
"""Build a complete Ripple compiler generation using only guest tools.

Uses a disposable Fuse-X image and a private shared output directory. The host
prepares the initial source/SDK snapshot and observes the guest, not its builds.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from test_hosted import Guest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def verify_rvm(generation, work):
    work.mkdir(parents=True)
    build = ROOT / 'build/fuzix'
    apps = 'init sh echo cat cp mkdir make picol tcl cmp'.split()
    for name in [*[n + '.rxe' for n in apps], 'FUZIX.SYS', 'bios.bin']:
        (work / name).symlink_to(build / ('picol.rxe' if name == 'tcl.rxe' else name))
    (work / 'apps.json').write_text(json.dumps(apps))
    (work / 'compiler').symlink_to(build / 'compiler', target_is_directory=True)
    files = [dict(guest='/generation/bin/' + name, host=str((generation / 'bin' / name).resolve()), mode=0o100755)
             for name in ['cc', 'rcc-c', 'rcc-ripple', 'rcc-as', 'rcc-ld']]
    manifest = work / 'files.json'; manifest.write_text(json.dumps(files))
    image = work / 'test.img'
    subprocess.run(['node', str(HERE / 'test_rvm_image.mjs'), 'create', str(image), str(work), str(manifest)], check=True)
    commands = 'tcl /usr/src/rcc/bootstrap.tcl check /generation\necho GENERATION_STATUS:$?\nshutdown\n'
    with (work / 'transcript.log').open('w') as log:
        child = subprocess.run([str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'), '--disk', str(image),
                                '--memory', '16384000', '--headless', '--jit', '--net', 'off',
                                '--input', commands, '--timeout', '3600'], stdout=log, stderr=subprocess.STDOUT, timeout=3620)
    text = (work / 'transcript.log').read_text().replace('\r\n', '\n')
    assert child.returncode == 0 and '\nGENERATION_STATUS:0\n' in text, text[-8000:]
    assert 'Compiled and linked entirely in the guest!' in text and 'Ripple: disk synced; power off.' in text, text[-8000:]
    print('Rebuilt compiler generation executed and compiled working programs under Riplix/RVM', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workdir', type=Path, default=HERE / 'build/guest-bootstrap')
    parser.add_argument('--target', choices=['build', 'tools'], default='build')
    parser.add_argument('--verify-rvm', type=Path, help='execute this complete compiler generation under Riplix/RVM')
    args = parser.parse_args()
    work = args.workdir.resolve()
    if args.verify_rvm:
        verify_rvm(args.verify_rvm.resolve(), work)
        return
    work.mkdir(parents=True, exist_ok=True)
    share = work / 'share'; share.mkdir(exist_ok=True)
    scratch = work / 'scratch'; scratch.mkdir(exist_ok=True)
    image = work / 'root.img'
    if image.exists():
        raise SystemExit(f'test image already exists: {image}; use a new --workdir')
    subprocess.run(['node', str(ROOT / 'fuse-x/image.mjs'), 'create', str(image),
                    '--apps', str(ROOT / 'fuse-x/build/apps')], check=True)
    trace = (work / 'transcript.log').open('wb')
    guest = Guest(ROOT / 'fuse-x/build/fuse-x', image, share, scratch, timeout=14400, trace=trace)
    try:
        guest.prompt()
        command = 'tcl /usr/src/rcc/bootstrap.tcl ' + args.target + ' /mnt/host/stage1'
        output = guest.command(command + '; echo BOOTSTRAP_STATUS:$?', 14000)
        assert '\nBOOTSTRAP_STATUS:0\n' in output, output[-8000:]
        expected = ['cc', 'rcc-as', 'rcc-ld'] + (['rcc-c', 'rcc-ripple'] if args.target == 'build' else [])
        for name in expected:
            assert (share / 'stage1/bin' / name).read_bytes()[:4] == b'RXE1', name
        files = {str(p): p.stat().st_mtime_ns for p in (share / 'stage1').rglob('*') if p.is_file()}
        output = guest.command(command + '; echo INCREMENTAL_STATUS:$?', 120)
        assert '\nINCREMENTAL_STATUS:0\n' in output, output[-8000:]
        assert files == {str(p): p.stat().st_mtime_ns for p in (share / 'stage1').rglob('*') if p.is_file()}, 'unchanged build rewrote outputs'
        guest.child.stdin.write(b'shutdown\n'); guest.child.stdin.flush()
        tail, _ = guest.child.communicate(timeout=20)
        assert guest.child.returncode == 0 and b'filesystem synced; session stopped' in tail, tail
    finally:
        guest.close()
        trace.close()
    print(f'Guest-only {args.target} generation and incremental rebuild passed: {share / "stage1/bin"}')


if __name__ == '__main__':
    main()
