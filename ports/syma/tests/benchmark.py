#!/usr/bin/env python3
"""Measure complete launches with many unrelated rules and a growing flat term."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

PORT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--binary', type=Path, default=PORT / 'build/host/syma')
args = p.parse_args()
records = []
with tempfile.TemporaryDirectory(prefix='syma-bench-') as tmp:
    source = Path(tmp) / 'bench.syma'
    for count in (0, 1000, 5000):
        rules = '\n'.join('{R "unused%d" {Unused%d x_} x_}' % (i, i) for i in range(count))
        source.write_text('''{Universe
 {Program {Tick 128}}
 {Rules %s
  {R "step" {Tick n_} {Tick {Sub n_ 1}} :guard {Gt n_ 0}}
  {R "end" {Tick 0} Done}}}''' % rules)
        elapsed = []
        for _ in range(3):
            start = time.monotonic()
            output = subprocess.check_output([str(args.binary.resolve()), 'run', str(source)], text=True)
            if '{Program Done' not in output:
                raise AssertionError(output)
            elapsed.append(time.monotonic() - start)
        records.append(dict(unrelated_rules=count, rewrites=129, median_ms=round(statistics.median(elapsed)*1000, 3)))
    source.write_text('{Universe {Program {Grow 3000 {}}} {Rules '
                      '{R step {Grow n_ {xs..}} {Grow {Sub n_ 1} {xs.. 1}} :guard {Gt n_ 0}} '
                      '{R end {Grow 0 {xs..}} {Size {xs..}}}}}')
    start = time.monotonic()
    output = subprocess.check_output([str(args.binary.resolve()), 'run', str(source)], text=True)
    if '{Program 3000' not in output:
        raise AssertionError(output)
    records.append(dict(flat_items=3000, milliseconds=round((time.monotonic()-start)*1000, 3)))
report = dict(binary=str(args.binary.resolve()), measurement='Complete process, parsing, indexing and normalization', records=records)
print(json.dumps(report, indent=2))
(PORT / 'build/benchmark-host.json').write_text(json.dumps(report, indent=2)+'\n')
