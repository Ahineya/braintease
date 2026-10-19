#!/usr/bin/env python3
"""Real guest DNS/TCP/TLS, asynchronous JSON, cache, viewer and failure cleanup."""
import argparse
from contextlib import closing
import hashlib
import json
from pathlib import Path
import sys
import tempfile

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from fixtures import make
from guest import Guest
from peer import Services


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('native', 'ripple'), required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--htget', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/quickjs-http-tests')
    args = parser.parse_args()
    build = args.build.resolve(); build.mkdir(parents=True, exist_ok=True)
    report = build / (args.target + '-http-report.json'); report.unlink(missing_ok=True)
    binary, worker = args.binary.resolve(), args.htget.resolve()
    with tempfile.TemporaryDirectory(prefix='quickjs-http-') as temp:
        directory = Path(temp); certificates = directory / 'certs'; make(certificates)
        body = json.dumps(dict(message='Привет 🌍', padding='x' * 70000), ensure_ascii=False).encode()
        with closing(Services(directory / 'nic', certificates, body=body, content_type=b'application/json')) as peer:
            guest = Guest(directory, args.target, {'js': binary, 'htget': worker}, peer=peer, blocks=65536,
                files=[dict(name='/etc/ssl/cert.pem', path=str(certificates / 'ca.pem')),
                       dict(name='/tmp/http/check.mjs', path=str(PORT / 'tests/http.mjs')),
                       dict(name='/tmp/http/demo.mjs', path=str(PORT / 'examples/fetch-json.mjs'))])
            try:
                output = guest.shell('js --max-time-ms 600000 --max-jobs 20000 /tmp/http/check.mjs /tmp/http; echo HTTP_STATUS:$?', 900)
                assert '\nHTTP CHECKS 26 PROGRESS TICKS ' in output and '\nHTTP_STATUS:0\n' in output, output
                print(args.target + ': ' + output[-500:], flush=True)
                demo = guest.shell('js --max-time-ms 180000 /tmp/http/demo.mjs https://tls.test/binary /tmp/http/demo.json; echo DEMO_STATUS:$?', 240)
                assert 'Cached /tmp/http/demo.json' in demo and 'Привет 🌍' in demo and '\nDEMO_STATUS:0\n' in demo, demo[-2000:]
                assert any(b'accept: application/json' in request.lower() for request in peer.requests)
            finally:
                (build / (args.target + '-http-guest.log')).write_bytes(guest.log)
                guest.close()
        result = dict(status='passed', target=args.target, assertions=26,
            executable_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
            worker_sha256=hashlib.sha256(worker.read_bytes()).hexdigest(),
            requests=peer.paths, tls_versions=peer.versions, filesystem_check='passed')
        report.write_text(json.dumps(result, indent=2) + '\n')
        print(str(report), flush=True)


if __name__ == '__main__': main()
