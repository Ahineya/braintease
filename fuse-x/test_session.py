#!/usr/bin/env python3
"""Exercise native applications through the real FUZIX process/syscall paths."""
import argparse
import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import tempfile
import termios
import time
from test_support import check_root


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--executable', type=Path, required=True)
    executable = parser.parse_args().executable.resolve()
    here = Path(__file__).resolve().parent
    environment = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off',
                       RCC_NATIVE_TIMEOUT='20')

    def run(image, commands, *, limit='16'):
        result = subprocess.run([str(executable), '--run', str(image), '--one-shot'], input=commands,
                                capture_output=True, text=True, timeout=25,
                                env=dict(environment, RCC_NATIVE_MAX_PROCESSES=limit))
        output = result.stdout + result.stderr
        assert result.returncode == 0, output
        assert 'filesystem synced; session stopped' in output, output
        assert 'CHECK FAILED' not in output and 'panic:' not in output, output
        return output

    with tempfile.TemporaryDirectory(prefix='fuse-x-session-') as temporary:
        image = Path(temporary) / 'root.img'
        subprocess.run(['node', str(here / 'image.mjs'), 'create', str(image), '--bootstrap-shell', '--apps', str(executable.parent / 'apps')], check=True)
        output = run(image, 'hello "two words" tail\ncheck\n'
                           'write /tmp/note "persistent value"\n'
                           'cat /tmp/note\nshutdown\n')
        assert 'Hello from native FUZIX! PID 3 two words tail' in output, output
        assert 'native process checks passed' in output, output
        assert 'persistent value\n' in output, output
        output = run(image, 'cat /tmp/note\nexit\n')
        assert 'persistent value\n' in output and 'shell stopped' in output, output
        # Upgrade the original small bootstrap geometry, including its inode
        # table, without losing a user file or its exact previous-image backup.
        builder = (here.parent / 'ports/fuzix/tools/disk.mjs').as_uri()
        for name,blocks,inodes in [('small',2048,128),('medium',32768,1024)]:
            small = Path(temporary) / (name+'.img')
            marker=name+'-root-kept'
            source = ('import fs from "node:fs"; import {rootImage} from ' + repr(builder) + ';'
                      'fs.writeFileSync(process.argv[1],rootImage([{name:"/tmp/kept",'
                      'mode:0o100644,data:Buffer.from(process.argv[2])}],'
                      +str(blocks)+','+str(inodes)+'));')
            subprocess.run(['node', '--input-type=module', '-e', source, str(small), marker], check=True)
            previous = small.read_bytes()
            subprocess.run(['node', str(here / 'image.mjs'), 'install', str(small),
                            '--bootstrap-shell', '--apps', str(executable.parent / 'apps')], check=True)
            assert small.stat().st_size == 65535 * 2048
            assert Path(str(small) + '.bak').read_bytes() == previous
            if name=='medium':
                # Place this preserved file at the final valid block to exercise
                # native I/O above the former 64 MiB image boundary.
                checker=(here.parent/'ports/fuzix/tools/fsck.mjs').as_uri()
                layout=(here.parent/'ports/fuzix/tools/layout.mjs').as_uri()
                relocate=('import fs from "node:fs"; import {checkRoot} from '+repr(checker)+';'
                          'import {inodeBase} from '+repr(layout)+';'
                          'let image=fs.readFileSync(process.argv[1]);'
                          'const at=image.indexOf(Buffer.from(process.argv[2],"utf16le"));'
                          'if(at<0 || at%2048)throw Error("missing preserved-file block");'
                          'const old=at/2048,high=65534;let moved=0;'
                          'for(let ino=1;ino<1024;ino++){const p=(inodeBase(ino)+12)*2;'
                          'if(image.readUInt16LE(p)===old){'
                          'image.copy(image,high*2048,at,at+2048);'
                          'image.writeUInt16LE(high,p);moved++;}}'
                          'if(moved!==1)throw Error("unexpected block ownership");'
                          'image=checkRoot(image,true).image;'
                          'if(checkRoot(image).issues.length)throw Error("bad relocated root");'
                          'fs.writeFileSync(process.argv[1],image);')
                subprocess.run(['node','--input-type=module','-e',relocate,str(small),marker],check=True)
            output = run(small, 'cat /tmp/kept\nwrite /tmp/expanded capacity-ok\n'
                               'cat /tmp/expanded\nshutdown\n')
            assert marker in output and 'capacity-ok' in output, output
            check_root(small)
        # Input substantially exceeds both the guest tty queue and host queue.
        output = run(image, ('echo ' + 'x' * 120 + '\n') * 40 +
                     'write /tmp/burst complete\nshutdown\n')
        assert output.count('x' * 120 + '\n') >= 40, 'piped commands were dropped'
        output = run(image, 'cat /tmp/burst\n')  # Host EOF also shuts down cleanly.
        assert 'complete\n' in output and 'shell stopped' in output, output
        output = run(image, 'hello\n' * 20 + 'shutdown\n', limit='2')
        assert output.count('fork: errno') == 20, output
        # Installing current manifests is idempotent and preserves user files.
        original = image.read_bytes()
        subprocess.run(['node', str(here / 'image.mjs'), 'install', str(image)], check=True)
        assert image.read_bytes() == original, 'unchanged manifests rewrote image'
        # Upgrade an older manifest without losing files, keeping its exact backup.
        outdated = original.replace('FXN1:hello:1'.encode('utf-16-le'),
                                    'FXN1:hello:0'.encode('utf-16-le'))
        assert outdated != original, 'manifest fixture was not found'
        image.write_bytes(outdated)
        subprocess.run(['node', str(here / 'image.mjs'), 'install', str(image)], check=True)
        assert image.with_suffix('.img.bak').read_bytes() == outdated
        output = run(image, 'cat /tmp/note\nshutdown\n')
        assert 'persistent value\n' in output, output
        master, slave = pty.openpty()
        saved = termios.tcgetattr(slave)
        child = subprocess.Popen([sys.executable, str(here / 'console.py'),
                                  str(executable), '--run', str(image), '--one-shot'],
                                 stdin=slave, stdout=slave, stderr=slave, env=environment)
        received = bytearray()

        def expect(marker):
            deadline = time.monotonic() + 10
            while marker not in received:
                assert time.monotonic() < deadline, received.decode(errors='replace')
                if select.select([master], [], [], 0.1)[0]:
                    received.extend(os.read(master, 4096))

        try:
            expect(b'fuse-x# ')
            # A real terminal sends CR; editing belongs to the FUZIX tty.
            os.write(master, b'echo terminal-ob\x7fk\r')
            expect(b'\r\nterminal-ok\r\n')
            os.write(master, b'exit\r')
            expect(b'session stopped')
            assert child.wait(timeout=5) == 0
            restored = termios.tcgetattr(slave)
            # macOS marks pending input for reprocessing after canonical restore.
            restored[3] &= ~getattr(termios, 'PENDIN', 0)
            saved[3] &= ~getattr(termios, 'PENDIN', 0)
            assert restored == saved, 'host terminal was not restored'
        finally:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=6)
            os.close(master)
            os.close(slave)
    print('Fuse-X: native init/shell, arguments, fork/wait/exec, COW, timers, '
          'slot reuse, console backpressure/EOF and persistence checks passed')


if __name__ == '__main__':
    main()
