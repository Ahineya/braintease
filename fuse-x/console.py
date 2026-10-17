#!/usr/bin/env python3
"""Run the native session with terminal settings restored on every exit."""
import os
import signal
import subprocess
import sys
import termios
import threading
import tty


def forward_diagnostics(stream, destination):
    """Host logs use LF; a raw terminal needs CRLF. Do not buffer prompts."""
    previous_cr = False
    while True:
        chunk = os.read(stream.fileno(), 4096)
        if not chunk:
            return
        output = bytearray()
        for byte in chunk:
            if byte == 10 and not previous_cr:
                output.append(13)
            output.append(byte)
            previous_cr = byte == 13
        destination.write(output)
        destination.flush()


def main():
    if not sys.stdin.isatty():
        os.execvp(sys.argv[1], sys.argv[1:])
    descriptor = sys.stdin.fileno()
    saved = termios.tcgetattr(descriptor)
    child = None
    diagnostics = None

    def interrupted(number, _frame):
        raise SystemExit(128 + number)

    try:
        signal.signal(signal.SIGTERM, interrupted)
        signal.signal(signal.SIGHUP, interrupted)
        environment = os.environ.copy()
        environment.setdefault('RCC_NATIVE_NET_EAGER', '1')
        tty.setraw(descriptor)
        child = subprocess.Popen(sys.argv[1:], env=environment,
                                 stderr=subprocess.PIPE if sys.stderr.isatty() else None)
        if child.stderr is not None:
            diagnostics = threading.Thread(target=forward_diagnostics,
                                           args=(child.stderr, sys.stderr.buffer), daemon=True)
            diagnostics.start()
        result = child.wait()
        if diagnostics is not None:
            diagnostics.join()
        return result
    finally:
        if child is not None and child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        if diagnostics is not None:
            diagnostics.join(timeout=1)
        termios.tcsetattr(descriptor, termios.TCSANOW, saved)


if __name__ == '__main__':
    sys.exit(main())
