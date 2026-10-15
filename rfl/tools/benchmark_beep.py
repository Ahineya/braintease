#!/usr/bin/env python3
"""Measure beep's math and float formatting separately, with identical sample rate.

python3 rfl/tools/benchmark_beep.py --baseline /tmp/rfl-before \
    --baseline-math /tmp/math-before.rfl --samples 128
Use --full --jit to run all 44,100 original samples. Output is captured and hashed.
The previous math module must be supplied for a compiler-plus-library comparison.
"""
import argparse
import hashlib
import json
import tempfile
from pathlib import Path
from benchmark import ROOT, measure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rfl')
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--baseline-math', type=Path)
    parser.add_argument('--samples', type=int, default=128)
    parser.add_argument('--full', action='store_true')
    parser.add_argument('--micro', action='store_true', help='Add arithmetic loops with an observable checksum')
    parser.add_argument('--jit', action='store_true')
    parser.add_argument('--runs', type=int, default=3)
    args = parser.parse_args()
    if args.runs < 1 or not 1 <= args.samples <= 44100:
        parser.error('runs must be positive and samples must be in 1..44100')
    if args.full and args.micro:
        parser.error('--micro uses the bounded sample count; omit --full')
    if bool(args.baseline) != bool(args.baseline_math):
        parser.error('--baseline and --baseline-math must be supplied together')
    original = (ROOT / 'rfl/examples/beep.rfl').read_text()
    source = original if args.full else original.replace(
        'for (i in 0..sample_rate)',
        f'let iterations: u32 = {args.samples};\n    for (i in 0..iterations)')
    variants = {'beep': source}
    if not args.full:
        # Keep calculations observable even if software-float helper calls later
        # gain purity metadata and become eligible for dead-code elimination.
        checksum_source = source.replace('    for (i in', '    let mut checksum = 0.0;\n    for (i in')
        no_print = checksum_source.replace('println(sample * 0.5 + 0.5);', 'checksum += sample * 0.5 + 0.5;')
        no_print = no_print[:no_print.rfind('}')] + '    println(checksum);\n}\n'
        phase = checksum_source[:checksum_source.index('        let sample =')]
        phase += '        checksum += t * 0.5 + 0.5;\n    }\n    println(checksum);\n}\n'
        variants.update({
            'math_checksum': no_print,
            'print_only': source[:source.index('        let t =')] + '        println(0.625);\n    }\n}\n',
            'phase_checksum': phase,
        })
    if args.micro:
        for name, expression in [('convert', 'x'), ('add', 'x + 0.125'),
                                 ('mul', 'x * 1.25'), ('div', 'x / 44100.0')]:
            variants['micro_' + name] = (
                'fn main() { let mut checksum = 0.0; '
                f'let count: u32 = {args.samples}; '
                'for (i in 0..count) { let x = i as f64; '
                f'checksum += {expression}; '
                '} println(checksum); }')
    results = {'samples': 44100 if args.full else args.samples, 'jit': args.jit, 'cases': {}}
    with tempfile.TemporaryDirectory(prefix='rfl-beep-') as temporary:
        root = Path(temporary)
        if args.baseline_math:
            (root / 'before_math.rfl').write_text(args.baseline_math.read_text())
        for name, case in variants.items():
            results['cases'][name] = {}
            for label, compiler in [('before', args.baseline), ('after', args.compiler)]:
                if compiler is None:
                    continue
                text = case.replace('"std/math"', '"./before_math"') if label == 'before' else case
                path = root / f'{name}_{label}.rfl'
                path.write_text(text)
                directory = root / f'{name}_{label}'
                directory.mkdir()
                result = measure(compiler.resolve(), path, directory, args.runs, True,
                                 vm_args=['--jit'] if args.jit else [], timeout=60)
                output = result.pop('stdout', '')
                result['output_bytes'] = len(output.encode())
                result['output_lines'] = output.count('\n')
                result['output_sha256'] = hashlib.sha256(output.encode()).hexdigest()
                results['cases'][name][label] = result
    print(json.dumps(results, indent=2))
    for case in results['cases'].values():
        if any(r.get('status') != 0 for r in case.values()):
            raise SystemExit('Benchmark failed; see results above')
        if 'before' in case and case['before']['output_sha256'] != case['after']['output_sha256']:
            raise SystemExit('Benchmark output changed; see results above')


if __name__ == '__main__':
    main()
