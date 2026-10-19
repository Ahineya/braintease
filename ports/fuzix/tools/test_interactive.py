#!/usr/bin/env python3
"""Exercise shell editing, Levee, and the larger process pool in live RVM terminals."""
import argparse
import errno
import os
from pathlib import Path
import pty
import select
import subprocess
import tempfile
import time

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
parser.add_argument('--engine', choices=('both', 'jit', 'interpreter'), default='jit', help='RVM engine coverage (default: jit)')
options = parser.parse_args()
engines = [True, False] if options.engine == 'both' else [options.engine == 'jit']
engine_label = 'JIT and interpreter' if options.engine == 'both' else options.engine
build = options.build.resolve()

class Terminal:
    def __init__(self, image, jit):
        self.master, slave = pty.openpty()
        command = [str(ROOT/'scripts/run-fuzix.sh'), '--run-only', '--disk', str(image), '--timeout', '600']
        if not jit: command.append('--no-jit')
        self.proc = subprocess.Popen(command, cwd=ROOT, env=dict(os.environ, FUZIX_BUILD=str(build)),
                                     stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
        os.close(slave)
        self.buffer = b''
        self.log = bytearray()
        self.jit = jit
        self.until(b'riplix# ')
    def until(self, token, timeout=45):
        deadline = time.monotonic() + timeout
        while token not in self.buffer:
            if time.monotonic() > deadline: raise AssertionError((token, self.buffer[-4000:]))
            if select.select([self.master], [], [], 0.2)[0]:
                try: data = os.read(self.master, 65536)
                except OSError as e:
                    if e.errno == errno.EIO: raise AssertionError(self.buffer[-4000:]) from e
                    raise
                assert data, self.buffer[-4000:]
                self.log.extend(data); self.buffer += data
        n = self.buffer.index(token) + len(token)
        result, self.buffer = self.buffer[:n], self.buffer[n:]
        return result.replace(b'\r', b'')
    def send(self, data): os.write(self.master, data)
    def shell(self, data):
        self.send(data.encode() + b'\r' if isinstance(data, str) else data)
        return self.until(b'riplix# ')
    def edit(self, name, keys, command='levee'):
        print(('JIT' if self.jit else 'Interpreter') + ': ' + command + ' ' + name, flush=True)
        self.send((command + ' ' + name).encode() + b'\r')
        self.until(b'\x1b[?1049h')
        self.until(b'\x1b[1;1H')
        self.send(keys)
        self.until(b'\x1b[?1049l')
        return self.until(b'riplix# ')
    def content(self, name):
        output = self.shell('cat ' + name)
        return output.split(b'\n', 1)[1][:-len(b'riplix# ')]
    def close(self):
        Path('/tmp/fuzix-interactive-' + ('jit' if self.jit else 'interpreter') + '.log').write_bytes(self.log)
        if self.proc.poll() is None: self.proc.kill(); self.proc.wait()
        os.close(self.master)

def exercise(image, jit):
    t = Terminal(image, jit)
    try:
        color = b'\x1b[32mP>\x1b[0m '
        t.send(b"PS1='\\[\\e[32m\\]P>\\[\\e[0m\\] '\r")
        t.until(color)
        t.send(b'echo ac\x1b[Db\x1b[F\r')
        output = t.until(color)
        assert b'\nabc\n' in output and b'\x1b[3C\x1b[J' in output, output
        t.send(b"PS1='HEADER\\n\\[\\e[32m\\]P>\\[\\e[0m\\] '\r")
        t.until(color)
        t.send(b'echo ac\x1b[Db\x1b[F')
        output = t.until(b'\r\x1b[11C')
        assert b'HEADER' not in output, output
        t.send(b'\r')
        assert b'\nabc\n' in t.until(color)
        t.send(b"PS1='riplix# '\r")
        t.until(b'\r\nriplix# ')
        assert b'argument: completion' in t.shell(b'hell\tcompletion\r')
        t.shell("echo FILE > /completion-note")
        assert b'\nFILE\n' in t.shell(b'cat /comple\t\r')
        t.shell('mkdir /comp-dir; echo INSIDE > /comp-dir/note')
        assert b'\nINSIDE\n' in t.shell(b'cat /comp-d\tno\t\r')
        t.shell("echo SPACE > '/space name'")
        assert b'\nSPACE\n' in t.shell(b"cat '/spa\t\r")
        assert b'\nSPACE\n' in t.shell(b'cat /spa\t\r')
        t.shell('touch /ab-one /ab-two')
        t.send(b'echo /ab-\t\t')
        t.until(b'ab-one'); t.until(b'ab-two'); t.until(b'riplix# ')
        t.send(b'\x03'); t.until(b'riplix# ')
        assert b'\n130\n' in t.shell('echo $?')
        t.shell("trap 'echo PROMPT_TRAP' 2")
        t.send(b'not_a_command\x03'); t.until(b'\nPROMPT_TRAP'); t.until(b'riplix# ')
        t.shell('trap 2')
        t.shell('echo HISTORY_MARK')
        assert b'\nHISTORY_MARK\n' in t.shell(b'\x1b[A\r')
        assert b'\nDRAFT_MARK\n' in t.shell(b'echo DRAFT_MARK\x1b[A\x1b[B\r')
        assert b'\nabc\n' in t.shell(b'echo ac\x1b[Db\x1b[F\r')
        assert b'\nac\n' in t.shell(b'echo abc\x1b[D\x1b[D\x1b[3~\x1b[F\r')
        assert b'\nHOME_MARK\n' in t.shell(b'cho HOME_MARK\x1b[He\x1b[F\r')
        assert b'\nCLEAR_MARK\n' in t.shell(b'garbage\x15echo CLEAR_MARK\r')
        assert b'\nWORD_MARK\n' in t.shell(b'echo WORD_MARK garbage\x17\r')
        # Exercise history eviction and wrapped-line cursor movement.
        for i in range(38): t.shell('echo H' + str(i))
        assert b'\nH37\n' in t.shell(b'\x1b[A\r')
        long = b'x' * 110
        assert b'\n' + long + b'Y\n' in t.shell(b'echo ' + long + b'Z\x1b[D\x1b[3~Y\r')
        t.shell('echo untouched > /cancelled')
        t.send(b'echo BAD > /cancelled\x03'); t.until(b'riplix# ')
        assert t.content('/cancelled') == b'untouched\n'
        # Six pipeline children plus the shell exceeded the old four-slot pool.
        output = t.shell('seq 1 5 | cat | cat | cat | cat | wc -l; echo POOL:$?')
        assert b'5' in [line.strip() for line in output.splitlines()] and b'POOL:0' in output, output
        # An external process must receive canonical input after the line editor.
        t.send(b'cat > /raw-restored\r'); t.until(b'cat > /raw-restored')
        t.send(b'guest line\r\x04'); t.until(b'riplix# ')
        assert t.content('/raw-restored') == b'guest line\n'
        print(('JIT' if jit else 'Interpreter') + ': shell input passed', flush=True)
        # Exercise Levee explicitly; the default vi now launches Rift.
        t.edit('/levee-note', b'ione\rtwo\rthree\r\x1b:wq\r')
        assert t.content('/levee-note') == b'one\ntwo\nthree\n'
        t.shell('chmod 640 /levee-note')
        t.edit('/levee-note', b'ddu:wq\r')
        assert t.content('/levee-note') == b'one\ntwo\nthree\n'
        t.edit('/levee-note', b'yyjp:wq\r')
        assert t.content('/levee-note') == b'one\ntwo\none\nthree\n'
        t.edit('/levee-note', b'/three\rrT:wq\r')
        assert t.content('/levee-note') == b'one\ntwo\none\nThree\n'
        t.edit('/levee-note', b':%s/one/ONE/g\r:wq\r', command='levee')
        assert t.content('/levee-note') == b'ONE\ntwo\nONE\nThree\n'
        assert b'-rw-r-----' in t.shell('ls -l /levee-note')
        t.edit('/levee-note', b'dd:q!\r')
        assert t.content('/levee-note') == b'ONE\ntwo\nONE\nThree\n'
        t.edit('', b'iunnamed\x03:w /levee-new\rA again\x1b:wq\r')
        assert b'unnamed' in t.content('/levee-new')
        # More than 512 native characters forces disk-backed undo/yank.
        t.edit('/levee-large', b'500ddu:wq\r')
        output = t.shell('cmp /levee-large /levee-original; echo UNDO:$?')
        assert b'UNDO:0' in output, output
        t.edit('/levee-native', b'ddu:wq\r')
        assert b'NATIVE:0' in t.shell('cmp /levee-native /levee-native-original; echo NATIVE:$?')
        t.edit('/levee-over', b':q!\r')
        output = t.shell('wc -c /levee-over')
        assert b'31000' in output, output
        assert b'3.4' in t.log and b'\\u1234' in t.log
        assert b'\nTTY_OK\n' in t.shell(b'bad\x15echo TTY_OK\r')
        t.send(b'\x04'); t.until(b'Ripple: disk synced; power off.')
        assert t.proc.wait(timeout=10) == 0
        print(('JIT' if jit else 'Interpreter') + ': Levee passed', flush=True)
    finally: t.close()
    subprocess.run(['python3', str(PORT/'tools/fsck.py'), str(image)], check=True)

with tempfile.TemporaryDirectory(prefix='fuzix-interactive-') as temporary:
    for jit in engines:
        image = Path(temporary) / ('jit.img' if jit else 'interpreter.img')
        subprocess.run(['node', str(PORT/'tools/disk.mjs'), str(build), str(image)], check=True)
        fixtures = r"""
import fs from 'node:fs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
const f=process.argv[1], disk=openImage(f), root=fatFile(disk,'ROOTFS.IMG');
const text=Array.from('line\n'.repeat(600),c=>c.charCodeAt(0));
atomicWrite(f,root.replace(installRoot(root.data,[
 {name:'/levee-large',mode:0o100644,data:text},
 {name:'/levee-original',mode:0o100644,data:text},
 {name:'/levee-native',mode:0o100644,data:[0x1234,0xffff,10,65,10]},
 {name:'/levee-native-original',mode:0o100644,data:[0x1234,0xffff,10,65,10]},
 {name:'/levee-over',mode:0o100644,data:Array(31000).fill(65)}
])));
"""
        subprocess.run(['node', '--input-type=module', '-e', fixtures, str(image)], cwd=ROOT, check=True)
        exercise(image, jit)
        # Persistence is checked by reopening each image, not by host text conversion.
        check = subprocess.run([str(ROOT/'target/release/rvm'),str(build/'bios.bin'),'--disk',str(image),
                                '--memory','16384000','--headless','--input',
                                'cat /levee-note\ncmp /levee-large /levee-original; echo REBOOT:$?\nshutdown\n',
                                '--timeout','120',*(['--jit'] if jit else [])],
                               stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=130)
        assert check.returncode==0 and b'REBOOT:0' in check.stdout and b'ONE\ntwo\nONE\nThree\n' in check.stdout.replace(b'\r', b''),check.stdout
print('Interactive shell, Levee, eight process slots, clean terminal restoration and reboot persistence passed; ' + engine_label)
