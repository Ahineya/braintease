#!/usr/bin/env python3
"""Execute tools compiled by the portable native backend in private x86 Riplix.

The assessed tool objects are host-bootstrapped. This checks that our backend
can implement its own tools; it is not a guest source self-rebuild claim.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent / 'qbe'))
from test_x86 import EXTRACTOR, file, run_guest


def discard_guest_disks(out, phase):
    """Remove only a completed test phase's disposable disks; retain logs."""
    if not phase or Path(phase).name != phase:
        raise ValueError('expected a simple guest phase name')
    for suffix in ['-root.img', '-vars.fd']:
        (out / (phase + suffix)).unlink(missing_ok=True)


def exercise(sdk, tools, out, artifacts, timeout, qbe, sdk_objects=None, keep_disks=False,
             frontend=None, assembler=None):
    """Run the supplied tool paths; callers record their bootstrap provenance."""
    sdk_objects = sdk_objects or sdk
    files = [file('/bin/' + name, path) for name, path in tools.items()]
    files += [file('/bin/rcc-c', frontend or HERE.parent / 'build/apps/rcc-c-x86.fxe'),
              file('/bin/qbe', qbe),
              file('/bin/rcc-x64-as', assembler or ROOT / 'build/guest-native-assessment/rcc-x64-as-x86.fxe')]
    for folder in ['include', 'tree']:
        files += [file('/tmp/' + str(p.relative_to(sdk)), p)
                  for p in (sdk / folder).rglob('*') if p.is_file()]
    files += [file('/tmp/' + name, sdk_objects / name)
              for name in ['libc.a', 'crt0-0.o', 'crt0-2.o', 'crt0-3.o']]
    source = artifacts / 'hello.c'
    source.write_text('#include <stdio.h>\nextern int answer(void);\n'
                      'int main(void){puts("NATIVE_BACKEND_TOOLS_OK");return answer();}\n')
    answer = artifacts / 'answer.c'
    answer.write_text('int answer(void){return 37;}\n')
    broken = artifacts / 'broken.c'
    broken.write_text('int main(void) { return ; this is invalid C; }\n')
    files += [file('/hello.c', source), file('/answer.c', answer), file('/broken.c', broken)]
    commands = [(name + ' --help; echo HELP:$?', 'HELP:0') for name in tools]
    commands += [
        ('rcc-native-cc --sdk /tmp -c /answer.c -o /tmp/answer.o; echo BUILD:$?', 'BUILD:0'),
        ('rcc-x64-ar -o /tmp/libanswer.a /tmp/answer.o; echo ARCHIVE:$?', 'ARCHIVE:0'),
        ('rcc-native-cc --sdk /tmp -c /hello.c -o /tmp/hello.o; echo BUILD:$?', 'BUILD:0'),
        ('rcc-native-cc --sdk /tmp /tmp/hello.o /tmp/libanswer.a -o /tmp/hello; echo BUILD:$?', 'BUILD:0'),
        ('/tmp/hello; echo EXEC:$?', 'NATIVE_BACKEND_TOOLS_OK\nEXEC:37'),
        ('echo invalid >/tmp/bad.o; echo preserved >/tmp/keep.a; '
         'rcc-x64-ar -o /tmp/keep.a /tmp/bad.o; echo REJECT:$?; cat /tmp/keep.a', 'REJECT:1\npreserved'),
        ('echo preserved >/tmp/keep; rcc-native-cc --sdk /tmp /broken.c -o /tmp/keep; '
         'echo REJECT:$?; cat /tmp/keep', 'REJECT:1\npreserved')]
    disk, checks = run_guest(out, 'native-backend-tools', files, commands, timeout)
    subprocess.run(['node', '--input-type=module', '-e', EXTRACTOR, str(disk), str(artifacts),
                    json.dumps(['hello', 'hello.o', 'answer.o', 'libanswer.a'])], cwd=ROOT, check=True)
    reference_archive = artifacts / 'libanswer.host.a'
    subprocess.run([str(HERE.parent / 'build/rcc-x64-ar'), '-o', str(reference_archive),
                    str(artifacts / 'answer.o')], check=True)
    if reference_archive.read_bytes() != (artifacts / 'libanswer.a').read_bytes():
        raise RuntimeError('guest/host archive differs on the same object')
    reference = artifacts / 'hello.host-link'
    subprocess.run([str(HERE.parent / 'build/rcc-x64-ld'), '--sdk', str(sdk_objects),
                    str(artifacts / 'hello.o'), str(artifacts / 'libanswer.a'), '-o', str(reference)], check=True)
    if reference.read_bytes() != (artifacts / 'hello').read_bytes():
        raise RuntimeError('guest/host program link differs on the same objects')
    _, reloads = run_guest(out, 'native-backend-tools-execute', [file('/bin/hello', artifacts / 'hello')],
                          [('hello; echo EXEC:$?', 'NATIVE_BACKEND_TOOLS_OK\nEXEC:37')], timeout)
    if not keep_disks:
        for phase in ['native-backend-tools', 'native-backend-tools-execute']:
            discard_guest_disks(out, phase)
    return dict(commands=checks, second_boot=reloads, host_archive_parity=True, host_link_parity=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=ROOT / 'build/portable-native-sdk')
    parser.add_argument('--tools', type=Path, default=ROOT / 'build/portable-native-self-assessment/current')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/portable-native-x64')
    parser.add_argument('--qbe', type=Path, default=ROOT / 'build/guest-native-assessment/qbe-x86.fxe')
    parser.add_argument('--timeout', type=int, default=1800)
    parser.add_argument('--keep-disks', action='store_true', help='retain successful disposable VM disks for debugging')
    args = parser.parse_args()
    sdk, tools, out = args.sdk.resolve(), args.tools.resolve(), args.output.resolve()
    report = tools / 'execution-result.json'
    report.unlink(missing_ok=True)
    names = ['rcc-qbe', 'rcc-x64-ld', 'rcc-native-cc', 'rcc-x64-ar']
    result = exercise(sdk, {name: tools / (name + '.fxe') for name in names},
                      out, tools, args.timeout, args.qbe, keep_disks=args.keep_disks)
    report.write_text(json.dumps(dict(status='passed', **result,
        provenance='tools compiled and linked by host portable native pipeline; not a guest source self-rebuild'), indent=2) + '\n')
    print('Portable-backend native tools compile/archive/link/execute inside x86 Riplix and after fresh boot')


if __name__ == '__main__':
    main()
