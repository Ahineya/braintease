#!/usr/bin/env python3
"""Compare Fuzix bulk loading/process copies against BULK_IO=0.

Build kernel/bios in a separate directory with BULK_IO=0, then pass it through
--fallback-build. All disks are temporary; user disks are never opened.
"""
import argparse
from launch import require_mmu
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=ROOT/'build/fuzix')
p.add_argument('--engine', choices=('both', 'jit', 'interpreter'), default='jit', help='RVM engine coverage (default: jit)')
p.add_argument('--fallback-build', type=Path, required=True)
a = p.parse_args()
require_mmu(a.build)
require_mmu(a.fallback_build)
engines = [True, False] if a.engine == 'both' else [a.engine == 'jit']
engine_label = 'JIT and interpreter' if a.engine == 'both' else a.engine
commands = ('hello coffee\nhello\nforkcheck --stress\nforkcheck --stress\npreemptcheck\n'
            'echo durable > /bulk-note\nsync\ncat /bulk-note\nrm /bulk-note\nshutdown\n')
with tempfile.TemporaryDirectory(prefix='fuzix-bulk-') as directory:
    temporary = Path(directory)
    counts = {}
    for fast, jit in [(fast, jit) for fast in (False, True) for jit in engines]:
        name = f'{"bulk" if fast else "fallback"}-{"jit" if jit else "interp"}'
        build = temporary/name
        build.mkdir()
        # Match app versions exactly; only BIOS/kernel differ between modes.
        for source in a.build.resolve().iterdir():
            if source.suffix == '.rxe' or source.name == 'apps.json':
                (build/source.name).symlink_to(source)
        selected = a.build if fast else a.fallback_build
        for filename in ['FUZIX.SYS', 'bios.bin']:
            shutil.copyfile(selected/filename, build/filename)
        image, report = build/'disk.img', build/'report.json'
        subprocess.run(['node', str(PORT/'tools/disk.mjs'), str(build), str(image)], check=True)
        args = [str(ROOT/'target/release/rvm'), str(build/'bios.bin'), '--disk', str(image),
                '--memory', '16384000', '--headless', '--input', commands,
                '--timeout', '240', '--report', str(report)]
        if jit:
            args.append('--jit')
        started = time.monotonic()
        result = subprocess.run(args, capture_output=True, text=True, timeout=250)
        elapsed = time.monotonic() - started
        output = result.stdout + result.stderr
        assert result.returncode == 0, output
        assert 'argument: coffee' in output and output.count('Hello from a separate') == 2, output
        assert output.count('forkcheck: data/stack isolation and user zero passed') == 2, output
        assert output.count('parent value=7 child exit=23') == 2, output
        assert output.index('child ran while parent computed') < output.index('parent computation finished'), output
        assert 'durable\n' in output and 'Ripple: disk synced; power off.' in output, output
        assert 'panic:' not in output and 'FAILED' not in output, output
        data = json.loads(report.read_text())
        assert data['success'] and data['stop_reason'] == 'halted', data
        counts[name] = data['instructions']
        # libccheck spins until alarm(1); its instruction count depends on host
        # throughput, not transfer cost. Retain that coverage in a separate run.
        signal_args = args.copy()
        signal_args[signal_args.index('--input') + 1] = 'libccheck\nshutdown\n'
        at = signal_args.index('--report')
        del signal_args[at:at + 2]
        signal_check = subprocess.run(signal_args, capture_output=True, text=True, timeout=250)
        signal_output = signal_check.stdout + signal_check.stderr
        assert signal_check.returncode == 0, signal_output
        assert 'libccheck passed' in signal_output, signal_output
        assert 'Ripple: disk synced; power off.' in signal_output, signal_output
        assert 'panic:' not in signal_output and 'FAILED' not in signal_output, signal_output
        subprocess.run(['python3', str(PORT/'tools/fsck.py'), str(image)], check=True)
        print(f'{name}: {elapsed:.3f}s, {counts[name]:,} guest instructions', flush=True)
    # Compare fixed guest work, excluding the separate host-timed alarm spin.
    # Wall-clock ratios remain informational.
    for jit in engines:
        label = 'jit' if jit else 'interp'
        assert counts[f'bulk-{label}'] < counts[f'fallback-{label}'] * 0.5, counts
    print('Fuzix bulk/fallback loading, nested fork, bank isolation, preemption, signals, user buffers and persistence passed; ' + engine_label)
