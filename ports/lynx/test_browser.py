#!/usr/bin/env python3
"""Lynx HTTP, DNS and interactive UI in disposable native/RVM guests."""
import argparse
import json
import re
from pathlib import Path
import tempfile

from guest import Guest, Screen
from http_peer import Services
from probe import PORT, ROOT
from remote import Remote


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=['native', 'ripple'], default='native')
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--kernel', type=Path, default=ROOT / 'fuse-x/build/fuse-x')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--ui-only', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='lynx-http-', dir='/tmp') as temporary:
        directory = Path(temporary)
        peer = Services(directory / 'nic')
        guest = None
        try:
            guest = Guest(args, directory, peer)
            guest.raw(guest.prompt)
            for path, required in ([] if args.ui_only else [('/', b'Network browser ready'), ('/redirect', b'Fragmented chunked HTML passed'),
                                   ('/chunked', b'Entities & links work.'), ('/relative/next', b'Relative navigation passed')]):
                output = guest.shell(f'LYNX_RESOURCE_REPORT=1 {guest.program} -dump -nolist -cfg={guest.pages}/lynx.cfg '
                                     f'http://browser.test{path}; echo HTTP_STATUS:$?')
                assert b'HTTP_STATUS:0\n' in output and required in output, output
                assert b'<html' not in output, output
                print(args.target + ': HTTP dump ' + path + ' passed', flush=True)
            if not args.ui_only:
                output = guest.shell(f'{guest.program} -dump -nolist -cfg={guest.pages}/lynx.cfg '
                                     'http://browser.test/oversize; echo LIMIT_STATUS:$?')
                assert b'LIMIT_STATUS:1\n' in output and b'64 KiB transfer limit' in output, output
                print(args.target + ': oversized dump fails explicitly', flush=True)
            guest.send(f'LYNX_RESOURCE_REPORT=1 {guest.program} -cfg={guest.pages}/lynx.cfg http://browser.test/; echo BROWSER_STATUS:$?\r'.encode())
            guest.visible('Network browser ready')
            guest.send(b'\x1b[C')
            guest.visible('Relative navigation passed')
            guest.send(b'\x1b[D')
            guest.visible('Network browser ready')
            print(args.target + ': interactive HTTP follow/back passed', flush=True)
            guest.send(b'\t')
            guest.visible('Textfield')
            guest.send(b'retro & C16+\t')
            guest.send(b'\x1b[C')
            guest.visible('Everything')
            guest.send(b'\x1b[B\r\t')
            guest.send(b'\r')
            guest.visible('GET form encoding passed')
            assert any(path.startswith('/search?') for path in peer.paths), peer.paths
            print(args.target + ': GET form text/select/encoding passed', flush=True)
            # Enter a URL longer than the TCP transmit buffer through raw UI
            # input (the shell's canonical command line is intentionally small).
            guest.goto('http://browser.test/request?padding=' + 'x' * 1500)
            guest.visible('Complete long request received')
            print(args.target + ': complete long HTTP request passed', flush=True)
            # A load can be interrupted without losing the browser session.
            guest.goto('http://browser.test/slow')
            guest.wait(lambda: '/slow' in peer.paths, 'slow HTTP request received')
            guest.send(b'z')
            guest.visible('Commands:')
            guest.goto('http://browser.test/long')
            guest.visible('Scroll row 000')
            guest.send(b' ')
            guest.visible('Scroll row 010')
            old_title_column = ''.join(guest.screen.lines[0]).find('Scrolling fixture')
            assert old_title_column >= 0
            guest.resize(30, 100)
            guest.send(b'\x0c')
            guest.wait(lambda: ''.join(guest.screen.lines[0]).find('Scrolling fixture') > old_title_column + 10, 'browser uses the wider terminal')
            guest.resize(24, 80)
            guest.send(b'\x0c')
            guest.wait(lambda: 0 <= ''.join(guest.screen.lines[0]).find('Scrolling fixture') < old_title_column + 10, 'browser returns to 80 columns')
            guest.goto('http://browser.test/')
            guest.visible('Network browser ready')
            print(args.target + ': cancellation, scrolling, resize and recovery passed', flush=True)
            guest.goto('http://browser.test/oversize')
            guest.visible('64 KiB transfer limit')
            guest.visible('NORMAL LINK')
            guest.goto('http://browser.test/notfound')
            guest.visible('Fixture page not found')
            guest.goto('https://browser.test/')
            guest.visible('does not contain support for HTTPS')
            guest.visible('Commands:')
            guest.goto('http://browser.test:9999/')
            guest.visible('Unable to connect to remote host')
            guest.visible('Commands:')
            start = guest.goto('http://missing.test/')
            # This lookup notice is transient: one PTY read can contain both
            # the message and its replacement by the normal command line.
            guest.wait(lambda: b'Unable to locate remote host' in guest.log[start:], 'DNS failure notice')
            guest.visible('Commands:')
            guest.goto('http://browser.test/')
            guest.visible('Network browser ready')
            print(args.target + ': transfer cap, 404, DNS/HTTPS errors and recovery passed', flush=True)
            guest.goto(f'file://{guest.pages}/local.html')
            guest.visible('Ripple browser')
            guest.send(b'\x1b[C')
            guest.visible('Following local links works')
            guest.send(b'\x1b[D')
            guest.visible('Ripple browser')
            for _ in range(2):
                guest.send(b'\x1a')
                suspended = guest.raw(guest.prompt)
                assert b'Stopped' in suspended, suspended
                guest.screen = Screen()
                guest.send(b'fg\r')
                guest.visible('Ripple browser')
            print(args.target + ': offline navigation and suspend/resume passed', flush=True)
            guest.send(b'q')
            guest.visible('Are you sure')
            guest.send(b'y')
            output = guest.raw(guest.prompt)
            heap = [int(n) for n in re.findall(rb'LYNX_HEAP_ARENA_CELLS:([0-9]+)', output)]
            assert heap and max(heap) < 750000, output
            (PORT / f'build/runtime-{args.target}.json').write_text(json.dumps({
                'target': args.target, 'heap_arena_high_water_cells': max(heap),
                'transfer_limit_octets': 65536, 'cache_documents': 2,
                'scenario': 'forms, long request, 150 rows, 70K rejection and errors',
                'stack_high_water': None,
            }, indent=2) + '\n')
            # Exercise a real Telnet PTY while another remote shell remains usable.
            guest.shell('telnetd &')
            guest.wait(lambda: b'telnetd: listening' in guest.log, 'Telnet server listening')
            first, second = Remote(guest, 50100), Remote(guest, 50101)
            first.send(f'{guest.program} -cfg={guest.pages}/lynx.cfg http://browser.test/; echo REMOTE_STATUS:$?\r\n'.encode())
            first.visible('Network browser ready')
            assert b'OTHER_SESSION_OK\n' in second.shell('echo OTHER_SESSION_OK')
            first.send(b'\x1b[C')
            first.visible('Relative navigation passed')
            first.send(b'q')
            first.visible('Are you sure')
            first.send(b'y')
            assert b'REMOTE_STATUS:0' in first.raw(b'fuzix# ')
            assert b'REMOTE_RESTORED\n' in first.shell('echo REMOTE_RESTORED')
            first.send(b'exit\r\n')
            second.send(b'exit\r\n')
            print(args.target + ': Telnet browsing and independent second shell passed', flush=True)
            guest.send(b'shutdown\r')
            guest.process.wait(timeout=20)
            assert guest.process.returncode == 0
        finally:
            if guest: guest.close()
            peer.close()
    print(args.target + ': Lynx guest browser passed')


if __name__ == '__main__':
    main()
