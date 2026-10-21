#!/usr/bin/env python3
"""Run Levee's production Unix key reader on a real PTY, with split sequences."""
import argparse
import json
import os
from pathlib import Path
import platform
import pty
import select
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'ports/fuzix/upstream/Applications/levee'
UP, DOWN, LEFT, RIGHT, IGNORE = 14, 15, 16, 17, 18


class Reader:
    def __init__(self, executable, count):
        self.master, slave = pty.openpty()
        self.proc = subprocess.Popen([str(executable), str(count)], stdin=slave,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        os.close(slave)
        self.buffer = b''
        assert self.line() == b'READY'

    def line(self, timeout=1.5):
        end = time.monotonic() + timeout
        while b'\n' not in self.buffer:
            remaining = end - time.monotonic()
            assert remaining > 0, ('key reader stalled', self.buffer)
            assert select.select([self.proc.stderr], [], [], remaining)[0], 'key reader stalled'
            data = os.read(self.proc.stderr.fileno(), 4096)
            assert data, ('reader exited', self.proc.poll(), self.buffer)
            self.buffer += data
        line, self.buffer = self.buffer.split(b'\n', 1)
        return line

    def send(self, data):
        os.write(self.master, data)

    def keys(self, expected):
        got = [int(self.line()) for _ in expected]
        assert got == expected, (got, expected)

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()
        self.proc.stderr.close()
        os.close(self.master)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'clang'))
    parser.add_argument('--result', type=Path)
    args = parser.parse_args()
    flags = ['-std=gnu89', '-O1', '-g', '-fsanitize=undefined',
             '-fno-sanitize-recover=all', '-Wno-return-type', '-DRIPPLE',
             '-DANSI=1', '-DVT52=0', '-I' + str(SOURCE)]
    compatible = Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk')
    if platform.system() == 'Darwin' and compatible.is_dir():
        flags += ['-isysroot', str(compatible)]
    results = []
    with tempfile.TemporaryDirectory(prefix='levee-keys-') as folder:
        folder = Path(folder)
        harness, executable = folder / 'reader.c', folder / 'reader'
        harness.write_text('''#define main levee_main
#include "unixcall.c"
#undef main
char erasechar, eraseline;
int redisplay(int clear) { return clear; }
int main(int argc, char **argv) {
    int n = atoi(argv[1]);
    initcon();
    fprintf(stderr, "READY\\n"); fflush(stderr);
    while (n-- > 0) { fprintf(stderr, "%d\\n", getKey()); fflush(stderr); }
    fixcon();
    return 0;
}
''')
        subprocess.run([args.cc, *flags, str(harness), '-o', str(executable)], check=True)

        def check(name, fragments, expected):
            reader = Reader(executable, len(expected))
            try:
                for delay, data in fragments:
                    if delay:
                        time.sleep(delay)
                    reader.send(data)
                reader.keys(expected)
                assert reader.proc.wait(timeout=2) == 0
                results.append(name)
            finally:
                reader.close()

        check('CSI arrows', [(0, b'\x1b[A\x1b[B\x1b[D\x1b[C')], [UP, DOWN, LEFT, RIGHT])
        check('SS3 arrows', [(0, b'\x1bOA\x1bOB\x1bOD\x1bOC')], [UP, DOWN, LEFT, RIGHT])
        check('fragmented CSI and SS3', [(0, b'\x1b'), (.01, b'['), (.01, b'A'),
                                        (0, b'\x1bO'), (.01, b'D')], [UP, LEFT])
        check('modified arrows', [(0, b'\x1b[1;2A\x1b[1;5C')], [UP, RIGHT])
        check('standalone Escape', [(0, b'\x1b')], [27])
        check('Escape then immediate ex command', [(0, b'\x1b:q!\r')], [27, 58, 113, 33, 13])
        check('incomplete CSI replay', [(0, b'\x1b[')], [27, 91])
        check('unknown escape replay', [(0, b'\x1bx')], [27, 120])
        check('double Escape replay', [(0, b'\x1b\x1b:q\r')], [27, 27, 58, 113, 13])
        check('unsupported function keys are one ignored key each',
              [(0, b'\x1bOP\x1b[15~\x1b[[Ax')], [IGNORE, IGNORE, IGNORE, 120])
        check('bounded unknown CSI', [(0, b'\x1b[' + b'1;' * 40 + b'Aq')],
              [IGNORE] * 6 + [113])

        # Build the editor too: tokens must produce real buffer motions, and
        # unsupported insert navigation must never change the saved contents.
        editor = folder / 'levee'
        names = ('blockio display editcor exec find globals insert main misc '
                 'modify move ucsd undo unixcall wildargs').split()
        build = subprocess.run([args.cc, *flags, '-DSIZE=30000',
                                # Legacy optionrec casts an int * to a union
                                # containing pointers. LP64 alignment differs
                                # from the editor's C16 ABI; leave source alone.
                                '-fno-sanitize=alignment',
                                '-Wno-implicit-function-declaration',
                                '-Wno-int-conversion', '-Wno-main-return-type',
                                *[str(SOURCE / (name + '.c')) for name in names],
                                '-o', str(editor)], capture_output=True)
        assert build.returncode == 0, build.stderr.decode()

        def edit(name, keys, expected):
            note = folder / 'note'
            note.write_bytes(b'abc\ndef\nghi\n')
            master, slave = pty.openpty()
            proc = subprocess.Popen([str(editor), str(note)], stdin=slave,
                                    stdout=slave, stderr=subprocess.PIPE,
                                    env=dict(os.environ, LINES='24', COLUMNS='80', LVRC=''))
            os.close(slave)
            output = bytearray()

            def until(token):
                end = time.monotonic() + 5
                while token not in output:
                    remaining = end - time.monotonic()
                    assert remaining > 0, (name, bytes(output[-1000:]), proc.poll())
                    assert select.select([master], [], [], remaining)[0], (name, 'editor stalled')
                    data = os.read(master, 16384)
                    assert data, (name, proc.poll(), proc.stderr.read())
                    output.extend(data)

            try:
                until(b'\x1b[1;1H')
                os.write(master, keys)
                until(b'\x1b[?1049l')
                assert proc.wait(timeout=5) == 0, proc.stderr.read()
                assert not proc.stderr.read(), 'editor sanitizer diagnostic'
                assert note.read_bytes() == expected, (name, note.read_bytes(), expected)
                results.append(name)
            finally:
                if proc.poll() is None:
                    proc.kill()
                proc.wait()
                proc.stderr.close()
                os.close(master)

        edit('command Right moves the saved buffer', b'\x1b[CrX:wq\r', b'aXc\ndef\nghi\n')
        edit('command Down moves the saved buffer', b'\x1b[BrX:wq\r', b'abc\nXef\nghi\n')
        edit('command Left moves the saved buffer', b'll\x1b[DrX:wq\r', b'aXc\ndef\nghi\n')
        edit('command Up moves the saved buffer', b'j\x1b[ArX:wq\r', b'Xbc\ndef\nghi\n')
        edit('SS3 command motions compose', b'\x1bOC\x1bOB\x1bOD\x1bOArX:wq\r',
             b'Xbc\ndef\nghi\n')
        edit('arrow operator motion', b'd\x1b[C:wq\r', b'bc\ndef\nghi\n')
        edit('insert arrows do not mutate text',
             b'iONE\x1b[A\x1b[B\x1b[D\x1b[CTWO\x1b:wq\r', b'ONETWOabc\ndef\nghi\n')
        edit('literal insertion also ignores navigation',
             b':set nobeautify\riONE\x1bOA\x1bOB\x1bOD\x1bOC\x1bOP\x1b[15~\x1b[[ATWO\x1b:wq\r',
             b'ONETWOabc\ndef\nghi\n')
    result = dict(status='passed', checks=results,
                  sanitizer='undefined (fatal); full editor excludes legacy optionrec alignment',
                  scope='production Unix key reader and editor on host PTYs; physical acceptance pending')
    if args.result:
        args.result.parent.mkdir(parents=True, exist_ok=True)
        args.result.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
