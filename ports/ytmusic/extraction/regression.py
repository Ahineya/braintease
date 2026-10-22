#!/usr/bin/env python3
"""Known YouTube extraction regressions: real guest outputs, negatives and timing."""
import argparse
import json
import math
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, default=ROOT / 'build/ytmusic-extraction')
    ap.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    ap.add_argument('--label', default='regression')
    ap.add_argument('--full', action='store_true', help='also run cold preprocessing, including negative controls; may take many minutes')
    ap.add_argument('--backend', choices=('ejs', 'youtubejs', 'both'), default='both')
    ap.add_argument('--memory-mode', choices=('checked', 'read-v1'), default='read-v1')
    ap.add_argument('--enforce-baseline', action='store_true', help='same-machine timing/heap gate; historical single runs are not portable benchmarks')
    ap.add_argument('--max-slowdown', type=float, default=1.25)
    args = ap.parse_args()
    if not math.isfinite(args.max_slowdown) or args.max_slowdown < 1:
        ap.error('--max-slowdown must be finite and >= 1')
    if not re.fullmatch(r'[A-Za-z0-9-]+', args.label):
        ap.error('--label must contain only letters, digits and hyphens')
    baseline = json.loads((HERE / 'performance-baseline.json').read_text())
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    suite = build / 'regression-report.json'
    suite.unlink(missing_ok=True)
    records = []
    failed = False
    backends = ['youtubejs', 'ejs'] if args.backend == 'both' else [args.backend]
    for cached in ([True, False] if args.full else [True]):
        for backend in backends:
            name = backend + ('-prepared' if cached else '-cold')
            command = [sys.executable, str(HERE / 'check.py'), '--build', str(build),
                '--compiler', str(args.compiler.resolve()), '--backend', backend,
                '--memory-mode', args.memory_mode, '--label', args.label]
            if cached:
                command.append('--cached')
            print('Regression: ' + name, flush=True)
            run = subprocess.run(command)
            suffix = ('-read-v1' if args.memory_mode == 'read-v1' else '')
            suffix += '-cached' if cached else ''
            suffix += '-youtubejs' if backend == 'youtubejs' else ''
            path = build / ('report' + suffix + '-' + args.label + '.json')
            record = dict(case=name, passed=run.returncode == 0 and path.exists(), report=str(path))
            if path.exists():
                report = json.loads(path.read_text())
                record['status'] = report['status']
                case = report['cases'][0]
                if cached and 'guest' in case:
                    old = baseline['cases'][name]
                    record['guest_seconds'] = case['guest_seconds']
                    record['wall_ratio_to_baseline'] = case['guest_seconds'] / old['guest_seconds']
                    record['peak_cell_ratio_to_baseline'] = case['guest_resources']['peak_cells'] / old['peak_cells']
                    # Compare only the same pinned input and memory mode.
                    comparable = case['fixture_sha256'] == old['fixture_sha256'] and args.memory_mode == baseline['native_memory_mode']
                    record['comparable'] = comparable
                    if args.enforce_baseline and (not comparable or
                        record['wall_ratio_to_baseline'] > args.max_slowdown or
                        record['peak_cell_ratio_to_baseline'] > args.max_slowdown):
                        record['passed'] = False
            failed |= not record['passed']
            records.append(record)
            temporary = suite.with_suffix('.tmp')
            complete = len(records) == len(backends) * (2 if args.full else 1)
            temporary.write_text(json.dumps(dict(status=('failed' if failed else 'passed') if complete else 'running', complete=complete,
                scope='Offline cold and prepared fixtures' if args.full else 'Prepared execution only; no cold extraction claim',
                cases=records), indent=2) + '\n')
            temporary.replace(suite)
    print(suite)
    if failed:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
