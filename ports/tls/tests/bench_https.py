#!/usr/bin/env python3
"""Measure guest HTTPS with one CA versus a full bundle on the same local peer.

The full bundle appends the fixture CA to the pinned public roots. No public
Internet timing or host TLS proxy is involved. Each sample is a new process;
redirects also expose repeated trust setup within one process.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from fixtures import make
from guest import Guest, ROOT
from peer import Services


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('native', 'ripple'), default='native')
    parser.add_argument('--client', choices=('htget', 'lynx', 'both'), default='both')
    parser.add_argument('--executable', type=Path, help='override the selected single client')
    parser.add_argument('--repeat', type=int, default=2)
    parser.add_argument('--trust', choices=('small', 'full', 'both'), default='both')
    parser.add_argument('--redirect', action='store_true')
    parser.add_argument('--output', type=Path, help='save raw profiles and wall timings as JSON')
    parser.add_argument('--sample', type=Path, help='macOS native CPU sample output directory (timings will be perturbed)')
    args = parser.parse_args()
    if args.repeat < 1 or (args.executable and args.client == 'both'):
        parser.error('use a positive repeat count and one client with --executable')
    if args.sample and (args.target != 'native' or not Path('/usr/bin/sample').exists()):
        parser.error('--sample requires native Fuse-X on macOS')
    clients = ['htget', 'lynx'] if args.client == 'both' else [args.client]
    native = args.target == 'native'
    build = ROOT / ('fuse-x/build/apps' if native else 'build/fuzix')
    suffix = '.fxe' if native else '.rxe'
    apps = {name: args.executable or build / (name + suffix) for name in clients}
    results = []
    with tempfile.TemporaryDirectory(prefix='tls-benchmark-', dir='/tmp') as temporary:
        directory = Path(temporary)
        fixtures = make(directory / 'certs')
        full = directory / 'full.pem'
        full.write_bytes((ROOT / 'ports/tls/roots/cert.pem').read_bytes() +
                         (fixtures / 'ca.pem').read_bytes())
        peer = Services(directory / 'nic', fixtures,
                        b'<html><title>TLS benchmark</title><p>Verified benchmark body.</p></html>', b'text/html')
        files = [dict(name='/small.pem', path=str(fixtures / 'ca.pem')),
                 dict(name='/full.pem', path=str(full)),
                 dict(name='/etc/lynx.cfg', path=str(ROOT / 'ports/lynx/fixtures/lynx.cfg'))]
        guest = None
        try:
            guest = Guest(directory, args.target, apps, files, peer, blocks=65536)
            for client in clients:
                for trust in (['small', 'full'] if args.trust == 'both' else [args.trust]):
                    for repeat in range(args.repeat):
                        path = '/redirect' if args.redirect else '/binary'
                        url = 'https://tls.test' + path
                        command = ('htget --cacert /' + trust + '.pem ' + url + ' /result'
                                   if client == 'htget' else
                                   'SSL_CERT_FILE=/' + trust + '.pem lynx -dump -nolist -cfg=/etc/lynx.cfg ' + url)
                        started = time.monotonic()
                        sampler = None
                        if args.sample:
                            args.sample.mkdir(parents=True, exist_ok=True)
                            sample = args.sample / (client + '-' + trust + '-' + str(repeat) + '.txt')
                            os.write(guest.master, ('RIPPLE_TLS_PROFILE=1 ' + command + '; echo BENCH_STATUS:$?\r').encode())
                            sampler = subprocess.Popen(['/usr/bin/sample', str(guest.process.pid), '10',
                                                        '-file', str(sample)], stdout=subprocess.DEVNULL,
                                                       stderr=subprocess.DEVNULL)
                            text = guest.read(guest.prompt, 300)
                        else:
                            text = guest.shell('RIPPLE_TLS_PROFILE=1 ' + command + '; echo BENCH_STATUS:$?', 300)
                        elapsed = time.monotonic() - started
                        if sampler:
                            deadline = time.monotonic() + 30
                            while sampler.poll() is None:
                                # The Ethernet fixture is driven by this thread;
                                # keep draining packets while sampling completes.
                                guest.pump()
                                if time.monotonic() >= deadline:
                                    sampler.kill()
                                    sampler.wait()
                                    raise AssertionError('CPU sampling timed out')
                            assert sampler.returncode == 0, 'CPU sampling failed'
                        assert 'BENCH_STATUS:0' in text, text
                        result = dict(target=args.target, client=client, trust=trust, repeat=repeat,
                                      redirect=args.redirect, wall_seconds=elapsed, profile=text)
                        results.append(result)
                        print(json.dumps(result), flush=True)
            assert not peer.failures, peer.failures
        finally:
            if guest:
                guest.close()
            peer.close()
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
