#!/usr/bin/env python3
"""Independent UBSan CSV job checks; --guest also runs a disposable C16 guest.

The guest-ready test needs no disk argument; an optional writable fixture path
adds chunked file I/O and cancellation checks. It never uses an OS image here.
"""
from pathlib import Path
import csv
import hashlib
import json
import math
import random
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
OUT = ROOT / 'build/lvgl/jobs-agent'
OUT.mkdir(parents=True, exist_ok=True)
flags = ['-std=c99', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=undefined', '-fno-sanitize-recover=all']
compiler = 'cc'
if sys.platform == 'darwin':
    compiler = '/usr/bin/clang'
    flags += ['-isysroot', '/Applications/Xcode.app/Contents/Developer/Platforms/'
              'MacOSX.platform/Developer/SDKs/MacOSX15.5.sdk']
binary = OUT / 'csv-jobs-host'
subprocess.run([compiler, *flags, str(PORT / 'csv.c'), str(PORT / 'csv-job-test.c'),
                '-o', str(binary)], check=True)
subprocess.run([str(binary), str(OUT / 'read-fixture.csv')], check=True, timeout=60)
rng = random.Random(3271)
tokens = ['0', '-0', '2', '2.0', '0002', '+.2e1', '1e200', '-1e-200',
          '0x1.8p1', '-0x1p3', '0x0.0000000000001p-900', ' 2\t']
for _ in range(700):
    tokens.append(format(rng.uniform(-9, 9), '.17g') + 'e' + str(rng.randrange(-200, 200)))
tokens += ['0' * 20000 + '2', '2.' + '0' * 20000, '1' + '0' * 20000 + 'e-20000']

def value(token):
    return float.fromhex(token.strip()) if 'x' in token.lower() else float(token)

assert all(math.isfinite(value(token)) and (value(token) == 0 or abs(value(token)) > 1e-300)
           for token in tokens)
fixture = OUT / 'numeric-fixture.csv'
with fixture.open('w', newline='') as stream:
    csv.writer(stream, lineterminator='\n').writerows((token, i) for i, token in enumerate(tokens))
for descending in (False, True):
    command = [str(binary), '--sort', str(fixture)] + (['descending'] if descending else [])
    result = subprocess.run(command, check=True, capture_output=True, text=True, timeout=60)
    actual = list(map(int, result.stdout.splitlines()))
    expected = sorted(range(len(tokens)), key=lambda i: value(tokens[i]), reverse=descending)
    assert actual == expected, next((pair for pair in zip(actual, expected) if pair[0] != pair[1]), None)
guest = None
if '--guest-build' in sys.argv or '--guest' in sys.argv:
    guest = OUT / 'csv-job-test.fxe'
    subprocess.run([sys.executable, str(ROOT / 'fuse-x/cc.py'), str(PORT / 'csv.c'),
                    str(PORT / 'csv-job-test.c'), '--static', '--float-format', '--build',
                    str(OUT / 'c16'), '-o', str(guest)], check=True)
report = dict(status='passed', sanitizer='undefined', numeric_cases=len(tokens),
              checks=['incremental copy/read/UTF-8/detection/count/decode/index phases',
                      'cancel load and sort without changing document or order',
                      'malformed UTF-8/CSV preserves old document',
                      'long numeric and lexical cells use resumable scans',
                      '100000 rows, stable numeric ascending/descending differential sorting',
                      'header toggles after transactional commit'],
              input_sha256={name: hashlib.sha256((PORT / name).read_bytes()).hexdigest()
                            for name in ('csv.c', 'csv.h', 'csv-job-test.c', 'csv-job-test.py')})
if guest:
    report['guest_test_sha256'] = hashlib.sha256(guest.read_bytes()).hexdigest()
if '--guest' in sys.argv:
    sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
    from guest import Guest
    with tempfile.TemporaryDirectory(prefix='graphite-csv-jobs-') as directory:
        machine = Guest(Path(directory), 'native', {'csv-jobs': guest})
        try:
            result = machine.shell('csv-jobs /csv-read-test.csv; echo CSV_JOB_STATUS:$?', seconds=180)
            assert 'CSV_JOBS passed' in result and 'CSV_JOB_STATUS:0' in result, result
        finally:
            machine.close()
            (OUT / 'guest.log').write_bytes(machine.log)
    report['guest_status'] = 'passed'
(OUT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print('CSV job differential numeric sorts passed:', len(tokens), 'rows, both directions')
