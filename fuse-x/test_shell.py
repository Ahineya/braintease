#!/usr/bin/env python3
"""Exercise the real Bourne shell, signals, pipelines, PTYs and job control."""
import argparse
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time
from test_support import record_runtime

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    args = parser.parse_args()
    executable = args.executable.resolve()
    build = executable.parent
    environment = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off', RCC_NATIVE_TIMEOUT='45')
    with tempfile.TemporaryDirectory(prefix='fuse-x-shell-') as temporary:
        directory = Path(temporary)
        image = directory / 'root.img'
        subprocess.run(['node', str(HERE / 'image.mjs'), 'create', str(image), '--apps', str(build / 'apps')], check=True)
        control = directory / 'control.fxe'
        subprocess.run(['python3', str(HERE / 'cc.py'), str(HERE / 'examples/control.c'),
                        '--compiler', str(args.compiler.resolve()), '--build', str(build), '-o', str(control)], check=True)
        subprocess.run(['node', str(HERE / 'image.mjs'), 'put', str(image), '/bin/control-check', str(control)], check=True)
        terminal = directory / 'terminal.fxe'
        subprocess.run(['python3', str(HERE / 'cc.py'), str(HERE / 'examples/terminals.c'),
                        '--compiler', str(args.compiler.resolve()), '--build', str(build), '-o', str(terminal)], check=True)
        subprocess.run(['node', str(HERE / 'image.mjs'), 'put', str(image), '/bin/terminal-check', str(terminal)], check=True)
        # A real script file exercises variables, loops, conditionals and input
        # buffering independently of the interactive editor.
        script = directory / 'script'
        script.write_text('for item in alpha beta gamma; do echo item-$item; done\n'
                          'if test -f /tmp/copied; then echo condition-ok; fi\n'
                          'echo `echo substitution-ok`\n')
        subprocess.run(['node', str(HERE / 'image.mjs'), 'put', str(image), '/tmp/script', str(script)], check=True)
        child = subprocess.Popen([str(executable), '--run', str(image)], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=environment)
        received, transcript = bytearray(), bytearray()

        def expect(marker, seconds=15):
            deadline = time.monotonic() + seconds
            while marker not in received:
                assert time.monotonic() < deadline, transcript.decode(errors='replace')
                if select.select([child.stdout], [], [], .1)[0]:
                    chunk = os.read(child.stdout.fileno(), 8192)
                    assert chunk, transcript.decode(errors='replace')
                    received.extend(chunk); transcript.extend(chunk)
            end = received.index(marker) + len(marker)
            del received[:end]

        def send(command):
            child.stdin.write(command); child.stdin.flush()

        def command(text, marker):
            send(text.encode() + b'\n'); expect(marker); expect(b'fuse-x# ')

        try:
            expect(b'fuse-x# ')
            command('echo native-shell', b'\r\nnative-shell\r\n')
            send(b'discard-this\x03'); expect(b'^C\r\n'); expect(b'fuse-x# ')
            command('echo prompt-recovered', b'\r\nprompt-recovered\r\n')
            command('echo pipeline-value | cat > /tmp/copied; cat < /tmp/copied', b'\r\npipeline-value\r\n')
            command('sh /tmp/script', b'\r\nsubstitution-ok\r\n')
            command('/tmp/script', b'\r\nsubstitution-ok\r\n')
            assert b'item-alpha\r\nitem-beta\r\nitem-gamma\r\n' in transcript
            assert b'condition-ok\r\n' in transcript
            command('control-check', b'continuation and signal checks passed')
            command('terminal-check', b'PTY session checks passed')
            command('trap "echo trap-ok" 10; kill -10 $$', b'\r\ntrap-ok\r\n')
            command('sleep 30 &', b'Running sleep 30')
            command('jobs', b'Running sleep 30')
            send(b'fg\n'); time.sleep(.15); send(b'\x1a')
            expect(b'Stopped sleep 30'); expect(b'fuse-x# ')
            command('bg', b'Running sleep 30')
            send(b'fg\n'); time.sleep(.15); send(b'\x03')
            expect(b'fuse-x# ')
            command('echo alive-after-interrupt', b'\r\nalive-after-interrupt\r\n')
            # Levee must enter its terminal screen, accept edits and save.
            send(b'levee /tmp/edited\n'); expect(b'\x1b[')
            time.sleep(.2); send(b'inative-editor\x1b:wq\n')
            expect(b'fuse-x# ')
            command('cat /tmp/edited', b'\r\nnative-editor\r\n')
            send(b'shutdown\n'); expect(b'session stopped')
            assert child.wait(timeout=10) == 0, transcript.decode(errors='replace')
            assert b'CHECK FAILED' not in transcript and b'panic:' not in transcript, transcript.decode(errors='replace')
        finally:
            if child.poll() is None:
                child.kill(); child.communicate()
    record_runtime(build, ['sh', 'levee'], 'integration tested')
    print('Fuse-X: real Bourne shell, scripts, pipes, signals, PTYs, job control and editor checks passed')


if __name__ == '__main__':
    main()
