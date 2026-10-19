#!/usr/bin/env python3
"""Exercise guest links, installed aliases and persistence on disposable disks."""
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


def run(command):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def check_disk(image, native):
    code = '''
import {openImage} from './ports/fuzix/tools/image.mjs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {checkRoot} from './ports/fuzix/tools/fsck.mjs';
const disk=openImage(process.argv[1]);
const result=checkRoot(process.argv[2]==='1'?disk:fatFile(disk,'ROOTFS.IMG').data);
if(result.issues.length)throw Error(result.issues.join('\\n'));
'''
    run(['node', '--input-type=module', '-e', code, image, int(native)])


def exercise(args, system, work):
    native = system == 'fuse-x'
    build = (args.native_build if native else args.build).resolve()
    if not args.skip_build:
        if native:
            run(['make', '-C', ROOT / 'fuse-x', 'BUILD=' + str(build), '-j4', build / 'fuse-x'])
            run(['python3', ROOT / 'fuse-x/cc.py', PORT / 'tools/symlinkcheck.c',
                 '--build', build, '-o', build / 'apps/symlinkcheck.fxe'])
            run(['python3', ROOT / 'fuse-x/apps.py', 'sh', 'echo', 'cp', 'ln', 'mv', 'ls', 'rm', 'cat', 'levee', 'vi', 'tar', 'find', 'mkdir', '--build', build])
        else:
            run(['make', '-C', PORT, 'BUILD=' + str(build), '-j4', 'kernel', 'bios',
                 *[build / (n + '.rxe') for n in 'init sh echo cat cp ln mv ls rm levee rift vi tar find mkdir symlinkcheck'.split()]])
    image, stage = work / 'disk.img', work / 'stage'
    stage.mkdir()
    names = 'sh echo cp ln mv ls rm cat levee rift vi tar find mkdir symlinkcheck'.split()
    if native:
        apps = stage / 'apps'; apps.mkdir()
        for name in ['native-hello', *names]:
            (apps / (name + '.fxe')).symlink_to(build / 'apps' / (name + '.fxe'))
        (stage / 'application-report.json').write_text(json.dumps([dict(name=n, status='built') for n in names]))
        run(['node', ROOT / 'fuse-x/image.mjs', 'create', image, '--apps', apps])
        share = work / 'host'; share.mkdir()
        (share / 'file').write_text('host')
        (share / 'hidden').symlink_to('file')
        (work / 'secret').write_text('secret')
        (share / 'escape').symlink_to(work / 'secret')
        command = [build / 'fuse-x', '--run', image, '--share', share]
    else:
        names += ['init']
        for name in [*(n + '.rxe' for n in names if n not in ('vi', 'ln', 'mv')), 'FUZIX.SYS']:
            (stage / name).symlink_to(build / name)
        (stage / 'apps.json').write_text(json.dumps(names))
        run(['node', PORT / 'tools/disk.mjs', stage, image])
        command = [ROOT / 'target/release/rvm', build / 'bios.bin', '--disk', image,
                   '--memory', '16384000', '--net', 'off', '--jit', '--timeout', '600']
    for boot in range(2):
        guest = Guest(command, system)
        try:
            guest.raw(guest.prompt)
            def shell(command, marker=b'LINK_STATUS:0'):
                output = guest.shell(command + '; link_status=$?; echo; echo LINK_STATUS:$link_status', seconds=120)
                assert b'\n' + marker + b'\n' in output, output.decode(errors='replace')
                return output
            if boot:
                assert b'SYMLINK_PERSIST_OK' in shell('symlinkcheck persist')
                assert b'edited-via-alias' in shell('cat /tmp/edited')
            else:
                assert b'SYMLINK_OK' in shell('symlinkcheck' + (' host' if native else ''))
                assert b' -> rift' in shell('ls -l /bin/vi')
                assert b' -> cp' in shell('ls -l /bin/ln')
                assert b' -> dir/file' in shell('ls -l /links')
                shell('ln -s dir/file /links/cli')
                assert b'saved' in shell('cat /links/cli')
                shell('ln -s /missing /links/cli', b'LINK_STATUS:1')
                shell('ln -sf absent /links/cli')
                assert b' -> absent' in shell('ls -l /links/cli')
                shell('mv /links/cli /links/cli-moved')
                assert b' -> absent' in shell('ls -l /links/cli-moved')
                shell('cp -R /links/dir /tmp/copied')
                assert b' -> ../dir/file' in shell('ls -l /tmp/copied/parent')
                shell('cp /links/relative /tmp/plain')
                assert b'saved' in shell('cat /tmp/plain')
                shell('cp /links/relative /links/dir/file', b'LINK_STATUS:1')
                shell('cp -P /links/relative /links/relative', b'LINK_STATUS:1')
                shell('cp -LR /links/dir/file /links/relative', b'LINK_STATUS:1')
                shell('ln -s /links/dir /tmp/remove-link; rm -r /tmp/remove-link')
                assert b'saved' in shell('cat /links/dir/file')
                # Traversal and archives retain links, including dangling ones.
                shell('ln -s . /links/dir/loop')
                output = shell('find /links/dir -type l')
                assert b'/links/dir/loop' in output and b'/links/dir/parent' in output, output
                assert b'/loop/' not in output, output
                shell('cd /links; tar -cf /tmp/links.tar dir dangling')
                shell('mkdir /tmp/unpack; cd /tmp/unpack; tar -xf /tmp/links.tar; cd /')
                assert b' -> .' in shell('ls -l /tmp/unpack/dir/loop')
                assert b' -> missing' in shell('ls -l /tmp/unpack/dangling')
                shell('echo unchanged > /tmp/outside')
                shell('rm /tmp/unpack/dir/file; ln -s /tmp/outside /tmp/unpack/dir/file')
                shell('cd /tmp/unpack; tar -xf /tmp/links.tar dir/file; cd /')
                assert b'unchanged' in shell('cat /tmp/outside')
                assert b'saved' in shell('cat /tmp/unpack/dir/file')
                shell('rm -r /tmp/unpack/dir; ln -s /links/dir /tmp/unpack/dir')
                shell('cd /tmp/unpack; tar -xf /tmp/links.tar dir/file', b'LINK_STATUS:1')
                shell('cd /')
                shell('echo original > /tmp/edited')
                guest.send(b'vi /tmp/edited\r')
                guest.visible('NORMAL')
                guest.send(b'iedited-via-alias ')
                guest.visible('edited-via-alias')
                guest.send(b'\x1b')
                guest.visible('NORMAL')
                guest.send(b':')
                guest.visible(': command')
                guest.send(b'wq\r')
                guest.raw(guest.prompt)
                assert b'edited-via-alias' in shell('cat /tmp/edited')
                if native:
                    output = shell('ls -a /mnt/host')
                    # Command echo cannot contain these names.
                    assert b'hidden' not in output and b'escape' not in output, output
                    assert not (share / 'newlink').exists()
                    assert (share / 'file').read_text() == 'host'
                    assert (share / 'hidden').is_symlink()
                    assert (work / 'secret').read_text() == 'secret'
            guest.send(b'shutdown\r')
            guest.process.wait(timeout=30)
            assert guest.process.returncode == 0
        finally:
            (ROOT / 'build/fuzix' / f'symlinks-{system}-{boot}.log').write_bytes(guest.log)
            guest.close()
        check_disk(image, native)
    print(system + ': link syscalls, ln/mv/vi aliases, live editor save, recursive copy/delete, host hiding, reboot persistence and clean filesystem passed', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os', choices=('fuzix', 'fuse-x', 'both'), default='both')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--native-build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--skip-build', action='store_true')
    args = parser.parse_args()
    for system in (['fuse-x', 'fuzix'] if args.os == 'both' else [args.os]):
        with tempfile.TemporaryDirectory(prefix='guest-symlinks-') as directory:
            exercise(args, system, Path(directory))


if __name__ == '__main__':
    main()
