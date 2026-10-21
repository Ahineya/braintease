#!/usr/bin/env python3
"""Separate browser startup/local rendering from HTTP and image handling.

Uses private disks and supplied binaries. Public tests are opt-in and use
the existing blind TCP relay (not vmnet throughput measurements).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import tempfile
import time

from bench_fetch import ROOT, TimedGuest, Services, image, make
from public_peer import Relay


class ObservedGuest(TimedGuest):
    def __init__(self, *args, **kwargs):
        self.events = []
        super().__init__(*args, **kwargs)

    def pump(self):
        before = len(self.log)
        super().pump()
        if len(self.log) != before:
            self.events.append((time.monotonic(), bytes(self.log[before:]).decode(errors='replace')))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=ROOT/'fuse-x/build/apps')
    p.add_argument('--repeat', type=int, default=3)
    p.add_argument('--public', nargs='+', metavar='URL')
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    os.environ['NANOX_KEEP_TTY'] = '1'
    for name in ('FUSE_X_FRAMES', 'FUSE_X_TEST_EVENT'):
        os.environ.pop(name, None)
    rows = []
    with tempfile.TemporaryDirectory(prefix='netsurf-browser-bench-', dir='/tmp') as folder:
        work = Path(folder)
        cert = make(work/'cert')
        peer = Relay(work/'nic') if args.public else Services(work/'nic', cert)
        peer.sock.settimeout(.001)
        trust = work/'roots.pem'
        trust.write_bytes((ROOT/'ports/tls/roots/cert.pem').read_bytes() + (cert/'ca.pem').read_bytes())
        files = [dict(name='/dev/gfx', mode=0o20600, device=0x400),
                 dict(name='/etc/ssl/cert.pem', path=str(trust)),
                 dict(name='/usr/share/netsurf/web.nfa', path=str(ROOT/'ports/netsurf/fonts/web.nfa'))]
        cases = []
        if args.public:
            cases = [(url, enabled, None) for url in args.public for enabled in (False, True)]
        else:
            peer.assets = {}
            for name, count, size in [('empty',0,32), ('tiny-eight',8,32), ('four',4,128), ('reject-eight',8,1024)]:
                body = b'<!doctype html><title>Browser timing</title><p>Browser performance fixture</p>' + b''.join(
                    f'<img src="/{name}-{i}.png" width="64" height="64">'.encode() for i in range(count))
                peer.assets['/'+name+'.html'] = (b'text/html', body)
                for i in range(count):
                    peer.assets[f'/{name}-{i}.png'] = (b'image/png', image(size))
                cases.extend((prefix+'/'+name+'.html', True, count) for prefix in ('', 'http://tls.test'))
            for path, (_, body) in peer.assets.items():
                host = work/path[1:]
                host.write_bytes(body)
                files.append(dict(name=path, path=str(host)))
        apps = {name:args.build.resolve()/(name+'.fxe') for name in ('netsurf','htget')}
        apps.update({name:ROOT/'fuse-x/build/apps'/(name+'.fxe') for name in ('nano-X','nxhostcheck','kill')})
        kernel = Path(os.environ.get('FUSE_X_TEST_KERNEL', ROOT/'fuse-x/build/fuse-x'))
        metadata = {}
        for name, path in dict(apps, kernel=kernel, **{p.name:p for p in (ROOT/'fuse-x/build/lib').glob('*.fxl.[0-9]')}).items():
            metadata[name] = dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        for name, path in list(apps.items()):
            pinned = work/(name+'.fxe')
            pinned.write_bytes(path.read_bytes())
            assert hashlib.sha256(pinned.read_bytes()).hexdigest() == metadata[name]['sha256']
            apps[name] = pinned
        guest = None
        try:
            guest = ObservedGuest(work, 'native', apps, files, peer, blocks=131072)
            guest.shell('NANOX_KEEP_TTY=1; export NANOX_KEEP_TTY')
            guest.shell('nano-X -p &');guest.shell('s=$!')
            if b'Nano-X ready' not in guest.log:guest.read(b'Nano-X ready')
            for repeat in range(args.repeat):
                for url, images, expected in cases:
                    at = len(guest.log);event_at = len(guest.events)
                    requests = len(peer.paths) if not args.public else len(peer.hosts)
                    connections = len(peer.connections) if not args.public else len(peer.hosts)
                    start = time.monotonic()
                    guest.shell('NETSURF_TRACE_IMAGES=1 NETSURF_TRACE_HTTP=1 netsurf --no-disk-cache --trace ' +
                                ('--images ' if images else '--no-images ') + shlex.quote(url) + ' &')
                    guest.shell('p=$!')
                    deadline = start + 120
                    while b'NETSURF_GUI_DOCUMENT ' not in guest.log[at:] and time.monotonic() < deadline:
                        guest.pump()
                    elapsed = time.monotonic() - start
                    log = bytes(guest.log[at:])
                    ready = re.findall(rb'NETSURF_GUI_READY (\d+)', log)
                    assert ready, log[-6000:]
                    events = [(round(t-start,4), text) for t,text in guest.events[event_at:]]
                    accumulated = ''
                    ready_seconds = None
                    for timestamp, part in events:
                        accumulated += part
                        if ready_seconds is None and 'NETSURF_GUI_READY ' in accumulated:
                            ready_seconds = timestamp
                    row = dict(repeat=repeat, url=url, images=images, wall_seconds=elapsed,
                        complete=b'NETSURF_GUI_DOCUMENT ' in log,
                        ready_seconds=ready_seconds,
                        paints=log.count(b'NETSURF_GUI_PAINT '),
                        decoded=len(re.findall(rb'NETSURF_IMAGE .*status=0', log)),
                        rejected=log.count(b'NETSURF_IMAGE_REJECT '),
                        connections=(len(peer.hosts) if args.public else len(peer.connections))-connections,
                        events=events)
                    if not args.public:
                        row['requests'] = len(peer.paths)-requests
                        assert row['complete'] and row['requests'] == (expected+1 if url.startswith('http') else 0), row
                        if 'reject-eight' not in url:assert row['decoded'] == expected, row
                    else:
                        row['hosts'] = peer.hosts[requests:]
                        row['failures'] = list(peer.failures)
                    rows.append(row)
                    print(json.dumps({k:v for k,v in row.items() if k != 'events'}), flush=True)
                    args.output.write_text(json.dumps(dict(binaries=metadata, samples=rows), indent=2)+'\n')
                    guest.shell('nxhostcheck ctl close '+ready[-1].decode())
                    assert 'BROWSER_STATUS:0' in guest.shell('wait $p; echo BROWSER_STATUS:$?')
            guest.shell('kill $s; wait $s')
        finally:
            try:
                if guest:
                    args.output.with_suffix('.log').write_bytes(guest.log)
                    guest.close()
            finally:
                peer.close()


if __name__ == '__main__':
    main()
