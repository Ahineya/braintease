#!/usr/bin/env python3
"""Read/write folder mounts against disposable host files and a private root."""
import argparse
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time
from test_support import check_root, record_runtime

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    args = parser.parse_args()
    executable = args.executable.resolve()
    compiler = args.compiler.resolve()
    build = executable.parent
    with tempfile.TemporaryDirectory(prefix='fuse-x-hostfs-') as temporary:
        directory = Path(temporary)
        share = directory / 'Mac folder with spaces'; share.mkdir()
        image = directory / 'root.img'
        subprocess.run(['node', str(HERE / 'image.mjs'), 'create', str(image),
                        '--apps', str(build / 'apps')], check=True)
        (share / 'input.bin').write_bytes(bytes(i & 255 for i in range(8193)))
        (directory / 'secret').write_bytes(b'OUTSIDE_EXPORT')
        (share / 'escape').symlink_to(directory / 'secret')
        (share / '123456789012345678901234567890').write_bytes(b'KEEP_LONG_PREFIX')
        for name, source in [('hostfs-check', HERE / 'examples/hostfs.c'),
                             ('outside-program', HERE / 'examples/hello.c')]:
            subprocess.run(['python3', str(HERE / 'cc.py'), str(source), '--compiler', str(compiler),
                            '--build', str(build), '-o', str(share / name)], check=True)
        environment = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off', RCC_NATIVE_TIMEOUT='90')
        # Use the actual launcher, preserving host paths containing spaces.
        guest = subprocess.Popen([str(HERE.parent / 'scripts/run-fuse-x.sh'), '--run-only',
            '--one-shot', '--disk', str(image), '--share', str(share)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env=dict(environment, FUSE_X_BUILD=str(build)))
        received = bytearray(); transcript = bytearray()

        def expect(marker):
            deadline = time.monotonic() + 20
            while marker not in received:
                assert time.monotonic() < deadline, transcript.decode(errors='replace')
                if select.select([guest.stdout], [], [], .1)[0]:
                    chunk = os.read(guest.stdout.fileno(), 8192)
                    assert chunk, transcript.decode(errors='replace')
                    received.extend(chunk); transcript.extend(chunk)
                    assert b'hostfs FAILED' not in transcript and b'panic:' not in transcript, transcript.decode(errors='replace')
            del received[:received.index(marker) + len(marker)]

        def send(command):
            guest.stdin.write(command.encode() + b'\n'); guest.stdin.flush()

        try:
            expect(b'fuse-x# ')
            send('/mnt/host/hostfs-check; echo CHECK_STATUS:$?')
            expect(b'hostfs: binary I/O'); expect(b'\r\nCHECK_STATUS:0\r\n'); expect(b'fuse-x# ')
            assert (share / 'append.txt').read_bytes() == b'firstsecond'
            assert int((share / 'append.txt').stat().st_mtime) == 1900000000
            assert (share / 'sparse.bin').read_bytes() == bytes(9)
            assert (share / 'sparse.bin').stat().st_mode & 0o777 == 0o600
            assert not (share / 'renamed.bin').exists() and not (share / 'linked.bin').exists()
            assert not (share / 'moved-directory').exists()
            assert (share / '123456789012345678901234567890').read_bytes() == b'KEEP_LONG_PREFIX'
            assert (directory / 'secret').read_bytes() == b'OUTSIDE_EXPORT'
            # A new host file and host-side replacement must be visible live.
            (share / 'host-note').write_text('HOST_CHANGE_ONE\n')
            send('cat /mnt/host/host-note'); expect(b'\r\nHOST_CHANGE_ONE\r\n'); expect(b'fuse-x# ')
            (share / 'replacement').write_text('HOST_REPLACED_FILE\n')
            (share / 'replacement').replace(share / 'host-note')
            send('cat /mnt/host/host-note'); expect(b'\r\nHOST_REPLACED_FILE\r\n'); expect(b'fuse-x# ')
            send('echo FROM_GUEST > /mnt/host/guest-note'); expect(b'fuse-x# ')
            assert (share / 'guest-note').read_bytes() == b'FROM_GUEST\n'
            send('echo SECOND > /mnt/host/guest-note'); expect(b'fuse-x# ')
            assert (share / 'guest-note').read_bytes() == b'SECOND\n'
            send('cp /mnt/host/input.bin /tmp/copy; cmp /mnt/host/input.bin /tmp/copy; echo COPY_STATUS:$?')
            expect(b'\r\nCOPY_STATUS:0\r\n'); expect(b'fuse-x# ')
            send('/mnt/host/outside-program shared-folder-exec')
            expect(b'Independent executable, PID '); expect(b' shared-folder-exec\r\n'); expect(b'fuse-x# ')
            send('sh -c "cd /mnt/host; pwd"'); expect(b'\r\n/mnt/host\r\n'); expect(b'fuse-x# ')
            send('levee /mnt/host/editor-note'); expect(b'\x1b[')
            time.sleep(.2)
            guest.stdin.write(b'iedited from Fuse-X\x1b:wq\n'); guest.stdin.flush()
            expect(b'fuse-x# ')
            assert (share / 'editor-note').read_text().strip() == 'edited from Fuse-X'
            send('shutdown'); expect(b'session stopped'); assert guest.wait(timeout=10) == 0
        finally:
            if guest.poll() is None: guest.kill(); guest.wait()
            Path('/tmp/fuse-x-hostfs-console.log').write_bytes(transcript)
        check_root(image)
        result = subprocess.run([str(executable), '--run', str(image)],
            input='cat /tmp/copy > /dev/null\nls /mnt/host\nshutdown\n', text=True,
            capture_output=True, env=environment, timeout=20)
        assert result.returncode == 0 and 'session stopped' in result.stdout, result.stdout + result.stderr
        assert 'HOST_REPLACED_FILE' not in result.stdout and 'outside-program' not in result.stdout
        check_root(image)
        # An older bootstrap root has no mount directory. The native kernel
        # must create it through ordinary guest mkdir, without image import.
        old = directory / 'old-root.img'
        builder = (HERE.parent / 'ports/fuzix/tools/disk.mjs').as_uri()
        source = ('import fs from "node:fs";import {rootImage} from ' + repr(builder) + ';'
                  'fs.writeFileSync(process.argv[1],rootImage([...["init","sh"].map(id=>'
                  '({name:id==="init"?"/init":"/bin/sh",mode:0o100755,'
                  'data:Buffer.from("FXN1:"+id+":1\\n")})),{name:"/bin/cat",mode:0o100755,'
                  'data:fs.readFileSync(process.argv[2])}],2048,128));')
        subprocess.run(['node', '--input-type=module', '-e', source, str(old), str(build / 'apps/cat.fxe')], check=True)
        result = subprocess.run([str(executable), '--run', str(old), '--share', str(share)],
            input='cat /mnt/host/host-note\nshutdown\n', text=True, capture_output=True,
            env=environment, timeout=20)
        assert result.returncode == 0 and 'HOST_REPLACED_FILE' in result.stdout, result.stdout + result.stderr
        check_root(old)
    record_runtime(build, ['levee', 'vi'], 'shared folder integration tested')
    print('Fuse-X: live read/write Mac folder, native exec, editor saves, host updates, boundaries and root persistence passed')


if __name__ == '__main__':
    main()
