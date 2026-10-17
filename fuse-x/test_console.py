#!/usr/bin/env python3
"""Check host diagnostics, unbuffered prompts and guest output on a real PTY."""
import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import termios
import time

HERE = Path(__file__).resolve().parent


def main():
    master, slave = pty.openpty()
    saved = termios.tcgetattr(slave)
    probe = ('import os,time; '
             'os.write(2,b"network: starting\\nnetwork: ready\\r\\nPassword: "); '
             'assert os.read(0,1)==b"x"; '
             'os.write(1,b"guest\\r\\n"); '
             'os.write(2,b"split\\r"); time.sleep(.05); '
             'os.write(2,b"\\nfinished\\n")')
    child = subprocess.Popen([sys.executable, str(HERE / 'console.py'),
                              sys.executable, '-c', probe],
                             stdin=slave, stdout=slave, stderr=slave)
    received = bytearray()

    def expect(marker):
        deadline = time.monotonic() + 5
        while marker not in received:
            assert time.monotonic() < deadline, received
            if select.select([master], [], [], .1)[0]:
                received.extend(os.read(master, 4096))

    try:
        # A prompt without a newline must be visible before input is supplied.
        expect(b'Password: ')
        assert b'network: starting\r\nnetwork: ready\r\n' in received, received
        os.write(master, b'x')
        expect(b'finished\r\n')
        assert child.wait(timeout=5) == 0, received
        assert b'guest\r\n' in received and b'split\r\n' in received, received
        assert b'\r\r\n' not in received, received
        restored = termios.tcgetattr(slave)
        for attributes in [saved, restored]:
            attributes[3] &= ~getattr(termios, 'PENDIN', 0)
        assert restored == saved, 'host terminal was not restored'
    finally:
        if child.poll() is None:
            child.kill(); child.wait()
        os.close(master); os.close(slave)
    result = subprocess.run([sys.executable, str(HERE / 'console.py'), sys.executable,
                             '-c', 'import os;os.write(2,b"host\\n");os.write(1,b"guest\\r\\n")'],
                            input=b'', capture_output=True, check=True)
    assert result.stderr == b'host\n' and result.stdout == b'guest\r\n', result
    print('Fuse-X: terminal log alignment, partial prompts, CRLF and terminal restoration passed')


if __name__ == '__main__':
    main()
