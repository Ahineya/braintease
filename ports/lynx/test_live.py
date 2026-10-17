#!/usr/bin/env python3
"""Opt-in guest HTTP browsing over macOS shared vmnet; uses private disks.

The guest performs DNS, TCP and HTTP itself. Requires the existing vmnet helper
(and its normal sudo authorization). No host HTTP proxy or TLS gateway is used.
"""
import argparse
from pathlib import Path
import tempfile
import time

from guest import Guest, Screen
from probe import ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=['native', 'ripple'], default='native')
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--kernel', type=Path, default=ROOT / 'fuse-x/build/fuse-x')
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--address', default='192.168.64.248', help='private vmnet guest address, distinct from running guests')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='lynx-live-', dir='/tmp') as directory:
        guest = Guest(args, Path(directory), network='shared')
        # Separate from normal .250 guests and from other live acceptance tests.
        guest.logfile = guest.logfile.with_name('live-' + args.target + '.log')
        try:
            guest.raw(guest.prompt)
            result = guest.shell('ifconfig eth0 ' + args.address)
            assert ('inet ' + args.address).encode() in result, result
            failures = []
            for url, expected in [('http://example.com/', b'This domain is for use in documentation'),
                                  ('http://wiby.me/', b'Wiby'),
                                  ('http://wiby.me/?q=retro', b'retro'),
                                  ('http://felix.plesoianu.ro/web/stark-retro.html', b'Stark Retro')]:
                guest.shell('host ' + url.split('/')[2])
                result = guest.shell(f'{guest.program} -dump -nolist -cfg={guest.pages}/lynx.cfg {url}; echo LIVE_STATUS:$?')
                body = result.split(b'\n', 1)[1].rsplit(b'LIVE_STATUS:', 1)[0]
                challenge = url.startswith('http://felix.plesoianu.ro/') and b'Please wait while your request is being verified' in body
                if b'LIVE_STATUS:0\n' not in result or (expected.lower() not in body.lower() and not challenge):
                    failures.append((url, result))
                    print(args.target + ': failed ' + url, flush=True)
                    continue
                print(args.target + ': guest rendered ' + url + (' (site returned a verification page)' if challenge else ''), flush=True)
            guest.screen = Screen()
            guest.send(f'{guest.program} -cfg={guest.pages}/lynx.cfg http://wiby.me/; echo LIVE_UI_STATUS:$?\r'.encode())
            guest.visible('surprise me')
            for _ in range(12):
                if 'Textfield' in guest.screen.text(): break
                guest.send(b'\t')
                deadline = time.monotonic() + .2
                while time.monotonic() < deadline: guest.pump()
            guest.visible('Textfield')
            guest.send(b'retro\t\r')
            guest.visible('Retro Network')
            guest.send(b'q')
            guest.visible('Are you sure')
            guest.send(b'y')
            assert b'LIVE_UI_STATUS:0' in guest.raw(guest.prompt)
            print(args.target + ': interactive Wiby GET search passed', flush=True)
            guest.send(b'shutdown\r')
            guest.process.wait(timeout=20)
            assert guest.process.returncode == 0
            assert not failures, failures
        finally:
            guest.close()
    print(args.target + ': Internet HTTP browsing passed')


if __name__ == '__main__': main()
