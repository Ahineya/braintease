#!/usr/bin/env python3
"""Check script chmod enforcement on both kernels using private disk images."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from test_symlinks import run, check_disk

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
sys.path.insert(0, str(ROOT / 'rfl/tools'))
from test_os import Guest


def exercise(args, system, work):
    native = system == 'fuse-x'
    build = (args.native_build if native else args.build).resolve()
    names = 'sh echo chmod cat cp ln execpermcheck'.split()
    if not args.skip_build:
        if native:
            run(['make', '-C', ROOT / 'fuse-x', 'BUILD=' + str(build), '-j4', build / 'fuse-x'])
            run(['python3', ROOT / 'fuse-x/apps.py', *names[:-1], '--build', build])
            run(['python3', ROOT / 'fuse-x/cc.py', PORT / 'tools/execpermcheck.c',
                 '--build', build, '-o', build / 'apps/execpermcheck.fxe'])
        else:
            run(['make', '-C', PORT, 'BUILD=' + str(build), '-j4', 'kernel', 'bios', 'rfl-sdk',
                 *[build / (n + '.rxe') for n in ['init', *names[:-1]]]])
            assembly, obj = work / 'probe.asm', work / 'probe.pobj'
            run([ROOT / 'target/release/rcc', 'compile', PORT / 'tools/execpermcheck.c',
                 '-DRIPPLE_MMU=1', '--bank-size', '64000',
                 *['-I' + str(p) for p in (PORT / 'ripple/include', PORT / 'upstream/Library/include',
                                          ROOT / 'runtime/include', PORT / 'ripple')], '-o', assembly])
            run([ROOT / 'src/ripple-asm/target/release/rasm', 'assemble', assembly, '-o', obj,
                 '--bank-size', '64000', '--max-immediate', '65535'])
            sdk = json.loads(run(['make', '-s', '-C', PORT, 'BUILD=' + str(build), 'rfl-sdk-info']))
            run([ROOT / 'src/ripple-asm/target/release/rlink', sdk['entry'], obj, *sdk['libraries'],
                 '-f', 'rxe', '--bank-size', '64000', '-o', build / 'execpermcheck.rxe'])
    stage = work / 'stage'; stage.mkdir()
    image = work / 'disk.img'
    if native:
        apps = stage / 'apps'; apps.mkdir()
        for name in ['native-hello', *names]:
            (apps / (name + '.fxe')).symlink_to(build / 'apps' / (name + '.fxe'))
        (stage / 'application-report.json').write_text(json.dumps([dict(name=n,status='built') for n in names]))
        run(['node', ROOT / 'fuse-x/image.mjs', 'create', image, '--apps', apps])
        share = work / 'share'; share.mkdir()
        (share / 'script').write_text('echo HOST_SCRIPT_RAN\n')
        (share / 'script').chmod(0o644)
        command = [build / 'fuse-x', '--run', image, '--share', share]
    else:
        for name in ['init', *names]:
            if name != 'ln': (stage / (name + '.rxe')).symlink_to(build / (name + '.rxe'))
        (stage / 'FUZIX.SYS').symlink_to(build / 'FUZIX.SYS')
        (stage / 'apps.json').write_text(json.dumps(['init', *names]))
        run(['node', PORT / 'tools/disk.mjs', stage, image])
        command = [ROOT / 'target/release/rvm', build / 'bios.bin', '--disk', image,
                   '--memory', '16384000', '--net', 'off', '--jit', '--timeout', '600']
    guest = Guest(command, system)
    try:
        guest.raw(guest.prompt)
        def shell(command):
            output = guest.shell(command + '; perm_status=$?; echo; echo PERM_STATUS:$perm_status', seconds=120)
            lines = output.decode(errors='replace').splitlines()
            status = next(int(s.split(':')[1]) for s in lines if s.startswith('PERM_STATUS:'))
            return status, lines
        def allowed(command, marker):
            status, lines = shell(command)
            assert status == 0 and marker in lines, (command,status,lines)
        def denied(command, marker):
            status, lines = shell(command)
            assert status != 0 and marker not in lines, (command,status,lines)
        assert shell("echo 'echo SCRIPT_BODY_RAN' > /tmp/script; chmod 644 /tmp/script")[0] == 0
        # Collect the reproduction before the independent syscall assertions.
        denied('/tmp/script', 'SCRIPT_BODY_RAN')
        denied("'/tmp/script'", 'SCRIPT_BODY_RAN')
        denied('PATH=/tmp:/bin script', 'SCRIPT_BODY_RAN')
        denied('sh -c /tmp/script', 'SCRIPT_BODY_RAN')
        allowed('sh /tmp/script', 'SCRIPT_BODY_RAN')
        allowed('. /tmp/script', 'SCRIPT_BODY_RAN')
        assert shell('chmod 755 /tmp/script')[0] == 0
        allowed('/tmp/script', 'SCRIPT_BODY_RAN')
        allowed('PATH=/tmp:/bin script', 'SCRIPT_BODY_RAN')
        assert shell('ln -s script /tmp/script-alias; chmod 644 /tmp/script')[0] == 0
        denied('/tmp/script-alias', 'SCRIPT_BODY_RAN')
        assert shell('chmod 100 /tmp/script')[0] == 0
        allowed('/tmp/script-alias', 'SCRIPT_BODY_RAN')
        assert shell('chmod 10 /tmp/script')[0] == 0
        allowed('/tmp/script', 'SCRIPT_BODY_RAN')
        assert shell('chmod 1 /tmp/script')[0] == 0
        allowed('/tmp/script', 'SCRIPT_BODY_RAN')
        assert shell('cp /bin/echo /tmp/native; chmod 644 /tmp/native')[0] == 0
        denied('/tmp/native NATIVE_BODY_RAN', 'NATIVE_BODY_RAN')
        assert shell('chmod 755 /tmp/native')[0] == 0
        allowed('/tmp/native NATIVE_BODY_RAN', 'NATIVE_BODY_RAN')
        status, lines = shell('execpermcheck')
        assert status == 0 and any(s.startswith('EXECPERM_OK') for s in lines), (status,lines)
        if native:
            denied('/mnt/host/script','HOST_SCRIPT_RAN')
            allowed('sh /mnt/host/script','HOST_SCRIPT_RAN')
            (share / 'script').chmod(0o755)
            allowed('/mnt/host/script','HOST_SCRIPT_RAN')
            (share / 'script').chmod(0o644)
            denied('/mnt/host/script','HOST_SCRIPT_RAN')
        guest.send(b'shutdown\r');guest.process.wait(timeout=30)
        assert guest.process.returncode == 0
    finally:
        (ROOT / 'build/fuzix' / ('exec-permissions-' + system + '.log')).write_bytes(guest.log)
        guest.close()
    check_disk(image,native)
    print(system + ': direct/PATH/quoted scripts, chmod toggles, symlink targets, explicit sh/source, binaries, root/user permissions and clean filesystem passed',flush=True)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--os',choices=('fuzix','fuse-x','both'),default='both')
    p.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
    p.add_argument('--native-build',type=Path,default=ROOT/'fuse-x/build')
    p.add_argument('--skip-build',action='store_true')
    args=p.parse_args()
    for system in (['fuse-x','fuzix'] if args.os=='both' else [args.os]):
        with tempfile.TemporaryDirectory(prefix='exec-permissions-') as temporary:
            exercise(args,system,Path(temporary))


if __name__ == '__main__': main()
