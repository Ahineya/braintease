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
from decimal import Decimal, InvalidOperation
from pathlib import Path
from benchmark import ROOT, measure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rfl')
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--baseline-math', type=Path)
    parser.add_argument('--baseline-runtime', type=Path, help='Saved runtime archive to link before cases')
    parser.add_argument('--max-output-error', type=Decimal,
                        help='Allow numeric output changes up to this absolute error per line')
    parser.add_argument('--samples', type=int, default=128)
    parser.add_argument('--full', action='store_true')
    parser.add_argument('--micro', action='store_true', help='Add arithmetic loops with an observable checksum')
    parser.add_argument('--f32', action='store_true', help='Also measure binary32 arithmetic (requires --micro)')
    parser.add_argument('--jit', action='store_true')
    parser.add_argument('--runs', type=int, default=3)
    args = parser.parse_args()
    if args.runs < 1 or not 1 <= args.samples <= 44100:
        parser.error('runs must be positive and samples must be in 1..44100')
    if args.full and args.micro:
        parser.error('--micro uses the bounded sample count; omit --full')
    if args.f32 and not args.micro:
        parser.error('--f32 requires --micro')
    if bool(args.baseline) != bool(args.baseline_math):
        parser.error('--baseline and --baseline-math must be supplied together')
    if args.baseline_runtime and not args.baseline:
        parser.error('--baseline-runtime requires --baseline')
    if args.max_output_error is not None and (not args.max_output_error.is_finite() or args.max_output_error < 0):
        parser.error('--max-output-error must be finite and nonnegative')
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
        for ty in ['f64', 'f32'] if args.f32 else ['f64']:
            for name, expression in [('convert', 'x'), ('add', 'x + 0.125'),
                                     ('mul', 'x * 1.25'), ('div', 'x / 44100.0')]:
                label = name if ty == 'f64' else ty + '_' + name
                variants['micro_' + label] = (
                    f'fn main() {{ let mut checksum: {ty} = 0.0; '
                    f'let count: u32 = {args.samples}; '
                    f'for (i in 0..count) {{ let x = i as {ty}; '
                    f'checksum += {expression}; '
                    '} println(checksum); }')
        for name in ['sqrt', 'floor']:
            variants['micro_' + name] = (
                f'import {{{name}}} from "std/math"; '
                'fn main() { let mut checksum = 0.0; '
                f'let count: u32 = {args.samples}; '
                'for (i in 0..count) { let x = (i as f64) + 0.125; '
                f'checksum += {name}(x); '
                '} println(checksum); }')
    results = {'samples': 44100 if args.full else args.samples, 'jit': args.jit, 'cases': {}}
    with tempfile.TemporaryDirectory(prefix='rfl-beep-') as temporary:
        root = Path(temporary)
        if args.baseline_math:
            (root / 'before_math.rfl').write_text(args.baseline_math.read_text())
        for name, case in variants.items():
            results['cases'][name] = {}
            outputs = {}
            for label, compiler in [('before', args.baseline), ('after', args.compiler)]:
                if compiler is None:
                    continue
                text = case.replace('"std/math"', '"./before_math"') if label == 'before' else case
                path = root / f'{name}_{label}.rfl'
                path.write_text(text)
                directory = root / f'{name}_{label}'
                directory.mkdir()
                result = measure(compiler.resolve(), path, directory, args.runs, True,
                                 vm_args=['--jit'] if args.jit else [], timeout=60,
                                 runtime=args.baseline_runtime if label == 'before' else None)
                output = result.pop('stdout', '')
                outputs[label] = output
                result['output_bytes'] = len(output.encode())
                result['output_lines'] = output.count('\n')
                result['output_sha256'] = hashlib.sha256(output.encode()).hexdigest()
                results['cases'][name][label] = result
            if 'before' in outputs and args.max_output_error is not None:
                before, after = outputs['before'].splitlines(), outputs['after'].splitlines()
                if len(before) != len(after):
                    raise SystemExit(f'{name}: output line count changed')
                maximum = Decimal(0)
                for line, (old, new) in enumerate(zip(before, after), 1):
                    if old == new:
                        continue
                    try:
                        a, b = Decimal(old), Decimal(new)
                    except InvalidOperation:
                        raise SystemExit(f'{name}:{line}: changed non-numeric output')
                    if not a.is_finite() or not b.is_finite():
                        raise SystemExit(f'{name}:{line}: changed non-finite output')
                    maximum = max(maximum, abs(a - b))
                results['cases'][name]['after']['maximum_output_error'] = str(maximum)
    print(json.dumps(results, indent=2))
    for case in results['cases'].values():
        if any(r.get('status') != 0 for r in case.values()):
            raise SystemExit('Benchmark failed; see results above')
        if 'before' in case and case['before']['output_sha256'] != case['after']['output_sha256']:
            if args.max_output_error is None or Decimal(case['after']['maximum_output_error']) > args.max_output_error:
                raise SystemExit('Benchmark output changed beyond tolerance; see results above')


if __name__ == '__main__':
    main()
