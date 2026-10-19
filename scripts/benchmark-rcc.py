#!/usr/bin/env python3
"""Measure a fixed Riplix corpus, optionally alternating a saved baseline binary.

Use release binaries built with the same Cargo.lock/profile. Runs are serial,
warm-cache samples; this does not measure clean or incremental whole builds.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
CORPUS = {
    'hello': ('ports/fuzix/ripple/hello.c', False),
    'gterm': ('ports/fuzix/ripple/gterm.c', False),
    'tty': ('ports/fuzix/upstream/Kernel/tty.c', True),
}


def flags(kernel):
    common = ['-DRIPPLE_MMU=1']
    if kernel:
        return common + ['-DRIPPLE_BULK_IO=1', '-DRIPPLE_CODE_BANKS=1024',
                         '-DBANK_SIZE=64000', '-Iports/fuzix/ripple',
                         '-Iports/fuzix/upstream/Kernel/include',
                         '-Iports/fuzix/upstream/Kernel/dev', '-Iruntime/include',
                         '-Iports/graphics/include', '-Ic-test/examples/rOS']
    return common + ['-DNO_64BIT', '-Iports/fuzix/ripple/include',
                     '-Iports/fuzix/upstream/Library/include', '-Iruntime/include',
                     '-Iports/fuzix/ripple', '-Iports/graphics/include']


def measure(command):
    # Separate files avoid pipe backpressure. wait4 gives per-child CPU/RSS,
    # unlike RUSAGE_CHILDREN's cumulative peak across previous samples.
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        start = time.perf_counter()
        process = subprocess.Popen(command, cwd=ROOT, stdout=stdout, stderr=stderr)
        _, status, usage = os.wait4(process.pid, 0)
        process.returncode = os.waitstatus_to_exitcode(status)
        elapsed = time.perf_counter() - start
        stdout.seek(0)
        stderr.seek(0)
        output, errors = stdout.read(), stderr.read().decode(errors='replace')
    if process.returncode:
        raise RuntimeError(f'{command}: exit {process.returncode}\n{errors}\n'
                           + output.decode(errors='replace'))
    stages = {}
    for line in errors.splitlines():
        if line.startswith('rcc-timing '):
            _, stage, seconds = line.split()
            stages[stage] = float(seconds)
    return output, dict(wall_seconds=elapsed,
                        cpu_seconds=usage.ru_utime + usage.ru_stime,
                        peak_rss_bytes=usage.ru_maxrss * (1 if platform.system() == 'Darwin' else 1024),
                        stages=stages)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, required=True,
                        help='rcc, or rcpp for --mode preprocess')
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--mode', choices=('compile', 'ir', 'preprocess'), default='compile')
    parser.add_argument('--samples', nargs='+', choices=CORPUS, default=list(CORPUS))
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--warmups', type=int, default=1)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.repetitions < 1 or args.warmups < 0:
        parser.error('repetitions must be positive and warmups nonnegative')
    binaries = {'candidate': args.compiler.resolve()}
    if args.baseline:
        binaries['baseline'] = args.baseline.resolve()
    report = dict(mode=args.mode, cache='warm; no filesystem cache eviction',
                  processes=1, compiler_threads='serial backend',
                  platform=platform.platform(), machine=platform.machine(), processor=platform.processor(),
                  logical_cpus=os.cpu_count(),
                  revision=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  local_changes=subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True),
                  rustc=subprocess.check_output(['rustc', '--version'], text=True).strip(),
                  cargo_lock_sha256=hashlib.sha256((ROOT / 'Cargo.lock').read_bytes()).hexdigest(),
                  binaries={key: dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
                            for key, path in binaries.items()},
                  warmups=args.warmups, repetitions=args.repetitions, samples={})
    with tempfile.TemporaryDirectory(prefix='rcc-benchmark-') as temporary:
        for name in args.samples:
            source, kernel = CORPUS[name]
            samples = {key: [] for key in binaries}
            expected = None
            for iteration in range(args.warmups + args.repetitions):
                order = list(binaries)
                if iteration % 2:
                    order.reverse()
                for key in order:
                    command = [str(binaries[key])]
                    if args.mode != 'preprocess':
                        command += ['compile']
                    command += [source, *flags(kernel)]
                    if args.mode != 'preprocess':
                        command += ['--bank-size', '64000']
                        if args.mode == 'ir':
                            command += ['--emit-ir']
                        else:
                            command += ['-o', str(Path(temporary) / f'{name}-{key}.asm')]
                        if key == 'candidate':
                            command += ['--timings']
                    output, timing = measure(command)
                    if args.mode in ('ir', 'preprocess'):
                        if expected is None:
                            expected = output
                        if output != expected:
                            raise RuntimeError(f'{name}: {key} output differs from earlier run')
                    if iteration >= args.warmups:
                        samples[key].append(timing)
            summary = {key: {metric: statistics.median(s[metric] for s in runs)
                             for metric in ('wall_seconds', 'cpu_seconds', 'peak_rss_bytes')}
                       for key, runs in samples.items()}
            report['samples'][name] = dict(source=source,
                                           source_sha256=hashlib.sha256((ROOT / source).read_bytes()).hexdigest(),
                                           flags=flags(kernel),
                                           runs=samples, median=summary,
                                           output_sha256=hashlib.sha256(expected).hexdigest() if expected is not None else None)
            wall = summary['candidate']['wall_seconds']
            speedup = f", {summary['baseline']['wall_seconds'] / wall:.2f}x faster" if 'baseline' in summary else ''
            print(f'{name}: {wall:.6f}s median{speedup}', flush=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
