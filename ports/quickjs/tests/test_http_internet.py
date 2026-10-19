#!/usr/bin/env python3
"""Opt-in public JSON milestone: blind TCP relay, all verified TLS in the guest."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest
from test_internet import Relay


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target', choices=('native', 'ripple'), required=True)
    p.add_argument('--binary', required=True, type=Path)
    p.add_argument('--htget', required=True, type=Path)
    p.add_argument('--host', default='httpbin.org')
    p.add_argument('--path', default='/json')
    p.add_argument('--build', type=Path, default=ROOT / 'build/quickjs-http-tests')
    a = p.parse_args()
    build = a.build.resolve(); build.mkdir(parents=True, exist_ok=True)
    report = build / (a.target + '-public-http-report.json'); report.unlink(missing_ok=True)
    url = 'https://' + a.host + a.path
    with tempfile.TemporaryDirectory(prefix='qjs-public-') as temporary:
        directory = Path(temporary)
        peer = Relay(directory / 'nic', a.host)
        guest = None
        try:
            guest = Guest(directory, a.target, {'js': a.binary.resolve(), 'htget': a.htget.resolve()}, peer=peer,
                blocks=65536, files=[dict(name='/etc/ssl/cert.pem', path=str(ROOT / 'ports/tls/roots/cert.pem')),
                    dict(name='/tmp/fetch-json.mjs', path=str(PORT / 'examples/fetch-json.mjs'))])
            import shlex
            output = guest.shell('js --max-time-ms 180000 /tmp/fetch-json.mjs ' + shlex.quote(url) +
                                 '; echo PUBLIC_JSON_STATUS:$?', 240)
            print(output, flush=True)
            assert 'Cached /tmp/js-fetch-cache.json' in output and '\nPUBLIC_JSON_STATUS:0\n' in output, output
        finally:
            if guest:
                (build / (a.target + '-public-http-guest.log')).write_bytes(guest.log)
                guest.close()
            peer.close()
    report.write_text(json.dumps(dict(status='passed', target=a.target, url=url,
        executable_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),
        worker_sha256=hashlib.sha256(a.htget.read_bytes()).hexdigest(),
        tls='guest TLS with pinned Mozilla roots; blind relay only', filesystem_check='passed'), indent=2) + '\n')


if __name__ == '__main__': main()
