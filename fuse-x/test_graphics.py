#!/usr/bin/env python3
"""Exercise hosted GX and native Nano-X against a disposable real-kernel image."""
import argparse
import json
import os
from pathlib import Path
import select
import re
import subprocess
import tempfile
import time
from test_support import check_root

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    args = parser.parse_args()
    executable = args.executable.resolve()
    with tempfile.TemporaryDirectory(prefix='fuse-x-graphics-') as temporary:
        work = Path(temporary)
        image = work / 'root.img'
        frame = work / 'frame.ppm'
        frames = work / 'frames'
        event_file = work / 'host-event.json'
        apps = work / 'apps'
        apps.mkdir()
        for package in (executable.parent / 'apps').glob('*.fxe'):
            (apps / package.name).symlink_to(package)
        (work / 'application-report.json').write_text(json.dumps([
            dict(name=p.stem, status='built') for p in apps.glob('*.fxe')]))
        subprocess.run(['node', str(HERE / 'image.mjs'), 'create', str(image), '--apps',
                        str(apps)], check=True)
        env = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off',
                   RCC_NATIVE_TIMEOUT='180', FUSE_X_FRAME=str(frame), FUSE_X_FRAMES=str(frames),
                   FUSE_X_TEST_EVENT=str(event_file))
        child = subprocess.Popen([str(executable), '--run', str(image), '--one-shot'],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 env=env)
        log = bytearray()

        def until(text, start=0, timeout=20):
            limit = time.monotonic() + timeout
            while text.encode() not in log[start:]:
                if time.monotonic() >= limit:
                    raise AssertionError(f'timeout waiting for {text!r}:\n{log.decode(errors="replace")}')
                ready, _, _ = select.select([child.stdout], [], [], .1)
                if ready:
                    data = os.read(child.stdout.fileno(), 65536)
                    if not data:
                        raise AssertionError(f'kernel exited ({child.poll()}):\n{log.decode(errors="replace")}')
                    log.extend(data)

        def command(text, marker=None):
            start = len(log)
            child.stdin.write((text + '\n').encode());child.stdin.flush()
            if marker:
                until(marker, start)
            until('fuse-x# ', start)
            return log[start:].decode(errors='replace')

        try:
            until('fuse-x# ')
            # Each invocation leaves a window for exit cleanup. More than the
            # window limit exposes a leak in exit or inherited FD cleanup.
            for _ in range(18):
                command('gxcheck', 'gxcheck passed')
            command('gxcheck --exec', 'gxcheck exec passed')
            command('nxdraw --check', 'NANOX_ENGINE_OK')
            pixels = frame.read_bytes()
            assert pixels.startswith(b'P6\n640 480\n255\n') and len(set(pixels[-640*480*3:])) > 20
            command("gterm /bin/sh -c 'echo HOSTED_PTY_OK > /tmp/gui-pty'")
            command('cat /tmp/gui-pty', 'HOSTED_PTY_OK')
            # The quota probe expects to be the sole client of the server.
            command('nano-X -p &')
            command('server=$!')
            until('Nano-X ready')
            deadline = time.monotonic() + 15
            while True:
                output = command('nxcheck')
                if 'NANOX_CLIENT_OK' in output:
                    assert 'FAILED' not in output, output
                    break
                assert time.monotonic() < deadline, log.decode(errors='replace')
                time.sleep(.1)
            def windows():
                return json.loads((frames / 'windows.json').read_text())

            def window(title, visible=True):
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if child.poll() is not None:
                        log.extend(child.stdout.read())
                        raise AssertionError('kernel exited during graphics operation:\n'+log.decode(errors='replace'))
                    found = [w for w in windows() if w['title'] == title and w['visible'] == visible]
                    if found:
                        return found[0]
                    time.sleep(.02)
                raise AssertionError(f'missing window {title}: {windows()}')

            def host_event(title, **event):
                temporary=event_file.with_suffix('.tmp')
                temporary.write_text(json.dumps(dict(window=window(title)['id'], **event)))
                temporary.replace(event_file)

            def advances(title, sequence):
                deadline=time.monotonic()+10
                while window(title)['sequence']<=sequence:
                    assert time.monotonic()<deadline,f'{title} stopped repainting'
                    time.sleep(.02)

            def pty_size(path, cols, rows):
                deadline=time.monotonic()+10
                while time.monotonic()<deadline:
                    output=command('cat '+path)
                    match=re.search(r'PTY_SIZE (\d+) (\d+) (\d+)',output)
                    if match and tuple(map(int,match.groups()[:2]))==(cols,rows):
                        return int(match.group(3))
                raise AssertionError(f'PTY did not resize to {cols}x{rows}: {output}')

            def pixel(title, x, y, colour, size=None):
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    w = window(title)
                    try:
                        data = (frames / f"{w['id']}.ppm").read_bytes()
                        header = re.match(rb'P6\n(\d+) (\d+)\n255\n', data)
                        width, height = map(int, header.groups())
                        at = header.end() + (y * width + x) * 3
                        if (not size or (width, height) == size) and tuple(data[at:at+3]) == colour:
                            return
                    except (FileNotFoundError, AttributeError):
                        pass
                    time.sleep(.02)
                raise AssertionError(f'wrong pixel {title} {x},{y}; registry={windows()}')

            command('nxcheck --paint &', 'PAINTCHECK_READY')
            command('paint=$!')
            until('PAINTCHECK_WAITING')
            pixel('Paint check', 4, 4, (255, 0, 0))
            pixel('Paint check', 10, 10, (0, 255, 0))
            command('kill -10 $paint', 'PAINTCHECK_COMMITTED')
            pixel('Paint check', 4, 4, (0, 0, 255))
            pixel('Paint check', 10, 10, (255, 255, 255))
            command('kill -10 $paint')
            command('wait $paint')
            output = command('nxhostcheck a &', 'ROOTLESS_A_READY')
            a, child_id = map(int, re.search(r'ROOTLESS_A_READY (\d+) (\d+)', output).groups())
            command('a=$!')
            output = command('nxhostcheck b &', 'ROOTLESS_B_READY')
            b = int(re.search(r'ROOTLESS_B_READY (\d+)', output).group(1))
            command('b=$!')
            pixel('Rootless-A', 1, 1, (255, 0, 0))
            pixel('Rootless-A', 10, 10, (0, 255, 0))
            pixel('Rootless-B', 10, 10, (0, 0, 255))
            command(f'nxhostcheck ctl resize {a} 160 112', 'ROOTLESS_CONTROL_OK')
            pixel('Rootless-A', 150, 100, (255, 0, 0), (160, 112))
            # Cross both former caps while another independent client lives.
            # Repeat shrink/grow to expose backing-store lifetime and clipping bugs.
            for width, height in ((1280, 800), (320, 192), (1920, 1080), (160, 112)):
                host_event('Rootless-A', resize=[width,height])
                pixel('Rootless-A', width-2, height-2, (255, 0, 0), (width, height))
                pixel('Rootless-A', 10, 10, (0, 255, 0))
                pixel('Rootless-B', 10, 10, (0, 0, 255))
            for title,colour in (('Rootless-A',(255,0,0)),('Rootless-B',(0,0,255))):
                host_event(title,resize=[1920,1080])
                pixel(title,1918,1078,colour,(1920,1080))
            # Both full-HD double buffers nearly fill the guest arena. Shrink
            # without allocating; a growth needing two replacements must fail
            # atomically and must not send a fatal client error.
            host_event('Rootless-A',resize=[1919,1079])
            pixel('Rootless-A',1917,1077,(255,0,0),(1919,1079))
            configuration=window('Rootless-A')['configuration']
            host_event('Rootless-A',resize=[1920,1080])
            deadline=time.monotonic()+10
            while window('Rootless-A')['configuration']==configuration:
                assert time.monotonic()<deadline,'resize refusal not acknowledged'
                time.sleep(.02)
            pixel('Rootless-A',1917,1077,(255,0,0),(1919,1079))
            pixel('Rootless-B',1918,1078,(0,0,255),(1920,1080))
            for title,size in (('Rootless-A',(160,112)),('Rootless-B',(128,96))):
                host_event(title,resize=list(size))
                deadline=time.monotonic()+10
                while (window(title)['width'],window(title)['height'])!=size:
                    assert time.monotonic()<deadline
                    time.sleep(.02)
            command(f'nxhostcheck ctl title {b} Rootless-renamed', 'ROOTLESS_CONTROL_OK')
            window('Rootless-renamed')
            command(f'nxhostcheck ctl hide {a}', 'ROOTLESS_CONTROL_OK')
            window('Rootless-A', False)
            command(f'nxhostcheck ctl show {a}', 'ROOTLESS_CONTROL_OK')
            pixel('Rootless-A', 10, 10, (0, 255, 0))
            command(f'nxhostcheck ctl reparent {child_id} 1', 'ROOTLESS_CONTROL_OK')
            pixel('Rootless-child', 1, 1, (0, 255, 0))
            pixel('Rootless-A', 10, 10, (255, 0, 0))
            command(f'nxhostcheck ctl reparent {child_id} {a}', 'ROOTLESS_CONTROL_OK')
            pixel('Rootless-A', 10, 10, (0, 255, 0))
            command(f'nxhostcheck ctl close {a}', 'ROOTLESS_A_CLOSED')
            command('wait $a')
            pixel('Rootless-renamed', 10, 10, (0, 0, 255))
            command(f'nxhostcheck ctl close {b}', 'ROOTLESS_B_CLOSED')
            command('wait $b')
            # Both graphical terminals must acknowledge real host requests and
            # notify their foreground PTY child, including above 80x30 cells.
            for terminal, initial in (('gterm',(80,30)),('nxterm',(64,20))):
                path='/tmp/'+terminal+'-size'
                command(f'{terminal} nxhostcheck pty-size {path} &')
                command('sizing=$!')
                window('Terminal')
                pty_size(path,*initial)
                for width,height in ((1280,800),(320,192),(1800,960)):
                    host_event('Terminal',resize=[width,height])
                    assert pty_size(path,width//8,height//16)>0
                    assert (window('Terminal')['width'],window('Terminal')['height'])==(width,height)
                host_event('Terminal',close=True)
                command('wait $sizing')
            # Native startx reuses the persistent service, starts independent
            # clients, and returns to the calling shell without running NanoWM.
            command('NANOX_TRACE=1')
            command('export NANOX_TRACE')
            command('NANOX_KEEP_TTY=1')
            command('export NANOX_KEEP_TTY')
            command('startx', 'Native Nano-X applications started')
            window('Applications')
            window('Terminal')
            processes = command('ps -A')
            assert 'nanowm' not in processes, processes
            command('nxclock &')
            command('clock=$!')
            clock_sequence=window('Clock')['sequence']
            command('nxeyes &')
            command('eyes=$!')
            eyes_sequence=window('Eyes')['sequence']
            advances('Clock',clock_sequence)
            advances('Eyes',eyes_sequence)
            assert window('Clock')['resizable'] is False
            window('Eyes')
            command('kill $clock')
            command('kill $eyes')
            command('wait $clock')
            command('wait $eyes')
            # NXDSKTOP companions are independent native applications. Dialog
            # result stdout must survive terminal detachment and close cleanly.
            for program, title, result in (
                ("nxmsg 'Native dialog' 'Hello from NXDSKTOP'", 'Native dialog', '/tmp/nxmsg-result'),
                ('nxselect', 'Select File', '/tmp/nxselect-result'),
            ):
                command(f'{program} > {result} &')
                command('dialog=$!')
                assert window(title)['resizable'] is False
                output = command(f"nxhostcheck find '{title}'", 'ROOTLESS_FOUND')
                dialog = int(re.search(r'ROOTLESS_FOUND (\d+)', output).group(1))
                command(f'nxhostcheck ctl close {dialog}', 'ROOTLESS_CONTROL_OK')
                command('wait $dialog')
                output = command(f'cat {result}', '[]')
                assert any(line.strip() == '[]' for line in output.splitlines()), output
            command('nxcalc &')
            command('calculator=$!')
            assert window('Calculator')['resizable'] is False
            pixel('Calculator', 6, 6, (41, 44, 41))
            output = command('nxhostcheck find Calculator', 'ROOTLESS_FOUND')
            calculator = int(re.search(r'ROOTLESS_FOUND (\d+)', output).group(1))
            command(f'nxhostcheck ctl close {calculator}', 'ROOTLESS_CONTROL_OK')
            command('wait $calculator')
            # The game uses the same child-window renderer and monotonic
            # deadlines as RVM, while retaining an independent native top-level.
            command('echo 40000 > /.nxtetris-score')
            command('nxtetris --seed 1853 &')
            command('tetris=$!')
            assert window('Tetris')['resizable'] is False
            pixel('Tetris', 145, 15, (0, 255, 0), (230, 260))
            output = command('nxhostcheck find Tetris', 'ROOTLESS_FOUND')
            tetris = int(re.search(r'ROOTLESS_FOUND (\d+)', output).group(1))
            command(f'nxhostcheck ctl close {tetris}', 'ROOTLESS_CONTROL_OK')
            command('wait $tetris')
            output = command('cat /.nxtetris-score', '40000')
            assert any(line.strip() == '40000' for line in output.splitlines()), output
            # A GUI child survives its terminal's close, and drops inherited
            # slave descriptors so that a later terminal can reuse the PTY.
            command('NANOX_KEEP_TTY=')
            command("nxterm /bin/sh -c 'nxclock & wait' > /tmp/gui-child.log 2>&1 &")
            window('Clock')
            # The newest Terminal is the launching parent. Query the server's
            # window tree rather than relying on optional redirected tracing.
            output = command('nxhostcheck find Terminal', 'ROOTLESS_FOUND')
            terminal = int(re.search(r'ROOTLESS_FOUND (\d+)', output).group(1))
            output = command('nxhostcheck find Clock', 'ROOTLESS_FOUND')
            clock_window = int(re.search(r'ROOTLESS_FOUND (\d+)', output).group(1))
            command(f'nxhostcheck ctl close {terminal}', 'ROOTLESS_CONTROL_OK')
            window('Clock')
            command("nxterm /bin/sh -c 'echo REUSED_PTY > /tmp/reused-pty'")
            command('cat /tmp/reused-pty', 'REUSED_PTY')
            command(f'nxhostcheck ctl close {clock_window}', 'ROOTLESS_CONTROL_OK')
            command('kill $server')
            command('wait $server')
            command('echo GRAPHICS_SESSION_SURVIVED', 'GRAPHICS_SESSION_SURVIVED')
            child.stdin.write(b'shutdown\n');child.stdin.flush()
            until('filesystem synced; session stopped')
            assert child.wait(timeout=5) == 0
            assert all(bad not in log for bad in (b'panic:', b'failed line', b'runtime error:', b'not found', b'NXCHECK_', b'did not start')), log.decode(errors='replace')
        finally:
            if child.poll() is None:
                child.terminate()
                try: child.wait(timeout=5)
                except subprocess.TimeoutExpired: child.kill();child.wait()
            (executable.parent / 'graphics-test.log').write_bytes(log)
        check_root(image)
        print('Fuse-X graphics: GX ownership, RGB565 drawing, host resize through full HD, allocation-pressure refusal/recovery, gterm/nxterm PTY size and SIGWINCH, Nano-X clients/timers/protocol, private top-levels/children, reparent/map/title/close and session survival passed')


if __name__ == '__main__':
    main()
