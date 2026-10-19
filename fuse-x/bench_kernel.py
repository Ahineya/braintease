#!/usr/bin/env python3
"""Native kernel benchmark on a private root, with reusable application bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'ports/fuzix/tools'))
from bench_foundations import build_probe, run, Guest, check_disk


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--probe', type=Path)
    parser.add_argument('--source', type=Path, default=ROOT / 'ports/fuzix/tools/kernelbench.c')
    parser.add_argument('--repetitions', type=int, default=5)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error('--repetitions must be positive')
    build = args.build.resolve()
    executable = (args.executable or build / 'fuse-x').resolve()
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    probe = output / 'kernelbench.fxe'
    if args.probe:
        if args.probe.resolve() != probe:
            probe.write_bytes(args.probe.read_bytes())
    else:
        build_probe(build, True, probe, args.source.resolve())
    measurements = []
    import re
    with tempfile.TemporaryDirectory(prefix='native-kernel-bench-') as temporary:
        work = Path(temporary); image = work / 'root.img'
        files = [dict(name='/init', data='FXN1:init:1\n')]
        files += [dict(name='/bin/' + name, host=str(build / 'apps' / (name + '.fxe')))
                  for name in ['sh', 'echo']]
        files += [dict(name='/bin/kernelbench', host=str(probe)),
                  dict(name='/dev/tty', mode=0o20666, device=0x200),
                  dict(name='/dev/null', mode=0o20666, device=0x100)]
        manifest = work / 'files.json'; manifest.write_text(json.dumps(files))
        run(['node', '--input-type=module', '-e', r'''
import fs from 'node:fs';
import {rootImage,atomicWrite} from './ports/fuzix/tools/disk.mjs';
const files=JSON.parse(fs.readFileSync(process.argv[1],'utf8'));
for(const f of files)if(f.host){f.data=fs.readFileSync(f.host);delete f.host;}
else if(typeof f.data==='string')f.data=Buffer.from(f.data);
atomicWrite(process.argv[2],rootImage(files,16384,256));
''', manifest, image])
        guest = Guest([executable, '--run', image], 'fuse-x')
        try:
            guest.raw(guest.prompt)
            for repetition in range(args.repetitions):
                text = guest.shell('kernelbench; echo BENCH_STATUS:$?', seconds=180).decode(errors='replace')
                assert 'PROBE_OK' in text and 'BENCH_STATUS:0\n' in text, text
                found = re.findall(r'MEASURE (\w+) (\d+) (\d+)', text)
                assert found, text
                measurements += [dict(name=name, count=int(count), microseconds=int(micros),
                                      repetition=repetition) for name, count, micros in found]
                print('Native repetition', repetition + 1, 'passed', flush=True)
            guest.send(b'shutdown\r'); guest.process.wait(timeout=30)
            assert guest.process.returncode == 0
        finally:
            (output / 'guest.log').write_bytes(guest.log); guest.close()
        check_disk(image, True)
    result = dict(executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
                  probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(), measurements=measurements,
                  median_microseconds_per_operation={name: statistics.median(
                      r['microseconds'] / r['count'] for r in measurements if r['name'] == name)
                      for name in sorted({r['name'] for r in measurements})})
    (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result['median_microseconds_per_operation'], indent=2))


if __name__ == '__main__':
    main()
