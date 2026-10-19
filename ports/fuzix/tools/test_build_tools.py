#!/usr/bin/env python3
"""Regression tests for guest make and Tcl, first using host-compiled binaries."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'


def run(command, **options):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True, **options)
    if result.returncode:
        raise AssertionError((command, result.returncode, result.stdout[-4000:], result.stderr[-4000:]))
    return result


MAKEFILE = '''VALUE ?= default
VALUE += appended
-include optional.mk
.PHONY: all FORCE
.DELETE_ON_ERROR:
all: a a b
\t@echo "$@|$<|$^|$(VALUE)" > result
broken:
\t@echo partial > broken
\t@false
FORCE:
\t@:
preserve: FORCE
\t@false
'''


def host_checks(work):
    flags = ['-std=c99', '-D_DEFAULT_SOURCE', '-D_DARWIN_C_SOURCE']
    if sys.platform == 'darwin':
        flags += ['-isysroot', run(['xcrun', '--sdk', 'macosx', '--show-sdk-path']).stdout.strip()]
    make, tcl = work / 'make', work / 'tcl'
    run(['cc', *flags, PORT / 'upstream/Applications/MWC/cmd/make.c', '-o', make])
    run(['cc', *flags, '-I', PORT / 'ripple', PORT / 'upstream/Applications/TCL/picol.c', '-o', tcl])
    longfile = work / 'long.txt'; longfile.write_text('L' * 35000 + '\n')
    fixture = (PORT / 'tools/build-tools.tcl').read_text()
    script = work / 'check.tcl'; script.write_text(fixture)
    result = run([tcl, script, work / 'probe', longfile])
    assert 'TCL_BUILD_CHECKS_PASSED' in result.stdout
    assert result.stdout.count('L') >= 35000
    sources = work / 'make-check'; sources.mkdir()
    for name in ['a', 'b']:
        path = sources / name; path.write_text(name); os.utime(path, (0, 0))
    (sources / 'all').write_text('phony collision')
    (sources / 'Makefile').write_text(MAKEFILE)
    run([make, '-r'], cwd=sources)
    assert (sources / 'result').read_text().strip() == 'all|a|a b|default appended'
    run([make, '-r', 'VALUE=command'], cwd=sources)
    assert (sources / 'result').read_text().strip().endswith('|command')
    failed = subprocess.run([str(make), '-r', 'broken'], cwd=sources, capture_output=True, text=True)
    assert failed.returncode and not (sources / 'broken').exists(), failed
    (sources / 'preserve').write_text('keep existing output')
    failed = subprocess.run([str(make), '-r', 'preserve'], cwd=sources, capture_output=True, text=True)
    assert failed.returncode and (sources / 'preserve').read_text() == 'keep existing output', failed
    print('make and Tcl host acceptance checks passed', flush=True)


def guest_checks(work, osname):
    sys.path.insert(0, str(ROOT / 'rcc-portable'))
    from test_hosted import Guest
    directory = work / osname; directory.mkdir()
    files = []
    def fixture(guest, text):
        path = directory / ('fixture-' + str(len(files)))
        path.write_text(text)
        files.append(dict(guest=guest, host=str(path)))
    fixture('/build-tools.tcl', (PORT / 'tools/build-tools.tcl').read_text())
    fixture('/long.txt', 'L' * 35000 + '\n')
    fixture('/make-fixture/Makefile', MAKEFILE)
    for name in ['a', 'b', 'all']:
        fixture('/make-fixture/' + name, name)
    binary = ROOT / ('fuse-x/build/execargscheck.fxe' if osname == 'fuse-x' else 'build/fuzix/execargscheck.rxe')
    files.append(dict(guest='/bin/execargscheck', host=str(binary), mode=0o100755))
    manifest = directory / 'files.json'; manifest.write_text(json.dumps(files))
    image = directory / 'test.img'
    command = 'tcl /build-tools.tcl /tmp/probe /long.txt /make-fixture; echo CHECK_STATUS:$?'
    if osname == 'fuse-x':
        run(['node', ROOT / 'fuse-x/image.mjs', 'create', image, '--apps', ROOT / 'fuse-x/build/apps'])
        run(['node', ROOT / 'rcc-portable/test_rvm_image.mjs', 'install-native', image, directory, manifest])
        share = directory / 'share'; share.mkdir(); scratch = directory / 'scratch'; scratch.mkdir()
        guest = Guest(ROOT / 'fuse-x/build/fuse-x', image, share, scratch, timeout=180)
        try:
            guest.prompt()
            text = guest.command(command, 150)
            assert 'GUEST_BUILD_CHECKS_PASSED' in text and '\nCHECK_STATUS:0\n' in text, text[-4000:]
            guest.child.stdin.write(b'shutdown\n'); guest.child.stdin.flush()
            guest.child.communicate(timeout=20)
            assert guest.child.returncode == 0
        finally:
            (ROOT / 'rcc-portable/build/build-tools-native.log').write_bytes(guest.transcript)
            guest.close()
    else:
        build = ROOT / 'build/fuzix'
        apps = 'init sh echo cat cp mkdir rm cmp false true make picol tcl'.split()
        for name in [*[n + '.rxe' for n in apps], 'FUZIX.SYS', 'bios.bin']:
            (directory / name).symlink_to(build / ('picol.rxe' if name == 'tcl.rxe' else name))
        (directory / 'apps.json').write_text(json.dumps(apps))
        run(['node', ROOT / 'rcc-portable/test_rvm_image.mjs', 'create', image, directory, manifest])
        result = run([ROOT / 'target/release/rvm', build / 'bios.bin', '--disk', image,
                      '--memory', '16384000', '--headless', '--jit', '--net', 'off',
                      '--input', command + '\nshutdown\n', '--timeout', '600'], timeout=620)
        text = result.stdout.replace('\r\n', '\n')
        (ROOT / 'rcc-portable/build/build-tools-rvm.log').write_text(text)
        assert 'GUEST_BUILD_CHECKS_PASSED' in text and '\nCHECK_STATUS:0\n' in text, text[-4000:]
        assert 'Ripple: disk synced; power off.' in text, text[-1000:]
    print(osname + ': make, Tcl and exec argument acceptance checks passed', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--guests', choices=['none', 'fuse-x', 'fuzix', 'all'], default='none')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='guest-build-tools-') as temp:
        work = Path(temp)
        host_checks(work)
        for osname in ['fuse-x', 'fuzix']:
            if args.guests in [osname, 'all']:
                guest_checks(work, osname)
