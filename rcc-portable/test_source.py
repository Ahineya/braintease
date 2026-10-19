#!/usr/bin/env python3
"""Source snapshot identities and guest bootstrap invalidation, without a VM."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from source import HERE, prepare, sdk_identity


def main():
    with tempfile.TemporaryDirectory(prefix='guest-source-') as temporary:
        work = Path(temporary)
        source = prepare(work / 'source')
        before = {str(p): (p.read_bytes(), p.stat().st_mtime_ns)
                  for p in source.rglob('*') if p.is_file()}
        prepare(source)
        assert before == {str(p): (p.read_bytes(), p.stat().st_mtime_ns)
                          for p in source.rglob('*') if p.is_file()}
        sdk = work / 'sdk'; sdk.mkdir()
        (sdk / 'header.h').write_text('version one')
        identity_path = work / 'sdk-identity'
        identity = sdk_identity(sdk, identity_path).read_text()
        (sdk / 'sdk-id').write_text(identity)
        assert sdk_identity(sdk, identity_path).read_text() == identity
        (sdk / 'header.h').write_text('version two')
        assert sdk_identity(sdk, identity_path).read_text() != identity
        (sdk / 'seed-id').write_text('seed identity\n')
        seed = work / 'seed'; seed.mkdir()
        for name in ['cc', 'rcc-c', 'rcc-ripple', 'rcc-as', 'rcc-ld']:
            (seed / name).touch()
        helper = work / 'helpers'; helper.mkdir()
        make = helper / 'make'
        make.write_text('#!/bin/sh\n[ "$FAIL_MAKE" != 1 ]\n')
        make.chmod(0o755)
        flags = ['-std=c99', '-D_DARWIN_C_SOURCE', '-D_DEFAULT_SOURCE']
        if sys.platform == 'darwin':
            flags += ['-isysroot', subprocess.check_output(
                ['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()]
        tcl = work / 'tcl'
        root = HERE.parent
        subprocess.run(['cc', *flags, '-I', str(root / 'ports/fuzix/ripple'),
                        str(root / 'ports/fuzix/upstream/Applications/TCL/picol.c'), '-o', str(tcl)], check=True)
        output = work / 'output'
        environment = dict(os.environ, RCC_SDK=str(sdk), PATH=str(helper) + ':' + os.environ['PATH'])
        command = [str(tcl), str(source / 'bootstrap.tcl'), 'build', str(output), str(seed)]
        def build(expected=0):
            result = subprocess.run(command, env=environment, capture_output=True, text=True)
            assert result.returncode == expected, result.stderr
        build()
        stamp = output / 'inputs.stamp'
        timestamp = stamp.stat().st_mtime_ns
        obj = output / 'obj/probe.rpo'; obj.write_text('old object')
        binary = output / 'bin/cc'; binary.write_text('old compiler')
        notes = output / 'bin/notes'; notes.write_text('preserve')
        build()
        assert obj.exists() and binary.exists() and stamp.stat().st_mtime_ns == timestamp
        (source / 'source-id').write_text('updated sources\n')
        build()
        assert not obj.exists() and not binary.exists() and notes.exists()
        obj.write_text('old object')
        environment['CFLAGS'] = '-DNEW_FLAG=1'
        build()
        assert not obj.exists()
        obj.write_text('old object')
        (sdk / 'sdk-id').write_text('updated SDK\n')
        build()
        assert not obj.exists()
        environment['FAIL_MAKE'] = '1'
        build(1)
    print('Source identities, unchanged builds, source/flag/SDK invalidation and failure propagation passed')


if __name__ == '__main__':
    main()
