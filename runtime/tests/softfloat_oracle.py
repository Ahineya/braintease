#!/usr/bin/env python3
"""Check integer-only softfloat against native IEEE round-to-nearest-even.

The host adapter maps Ripple's 32-bit long to uint32_t/int32_t. This tests the
algorithms, not RCC code generation; the rct float tests cover the actual VM.
No native floating-point operations are added to the guest runtime.
"""
import argparse
import ctypes
import math
import os
import random
import re
import struct
import subprocess
import tempfile
from decimal import Decimal, localcontext
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def check_pi_table(source):
    """Independently regenerate the fixed-point table at high precision."""
    with localcontext() as context:
        context.prec = 430
        def atan_reciprocal(n):
            x = Decimal(1) / n
            term = result = x
            k = 1
            while abs(term) > Decimal('1e-420'):
                term = -term * x * x
                result += term / (2 * k + 1)
                k += 1
            return result
        pi = 16 * atan_reciprocal(5) - 4 * atan_reciprocal(239)
        expected = int((Decimal(2) / pi) * (1 << 1152))
    table = source.split('two_over_pi[72] = {', 1)[1].split('}', 1)[0]
    words = [int(word, 16) for word in re.findall(r'0x[0-9A-Fa-f]+', table)]
    assert len(words) == 72 and sum(word << (16 * i) for i, word in enumerate(words)) == expected, \
        '2/pi table differs from the high-precision reference'
    print('2/pi table: all 1152 bits agree with high-precision reference')


def adapter(source):
    for old, new in [('unsigned long long', 'uint64_t'), ('long long', 'int64_t'),
                     ('unsigned long', 'uint32_t'), ('long', 'int32_t')]:
        source = re.sub(r'\b' + old + r'\b', new, source)
    source = re.sub(r'\b(0[xX][0-9a-fA-F]+|[0-9]+)UL\b', r'\1U', source)
    return '#include <stdint.h>\n' + source


def reference(bits):
    ty, integer = ('float', 'uint32_t') if bits == 32 else ('double', 'uint64_t')
    code = f'typedef union {{ {ty} f; {integer} u; }} bits{bits};\n'
    for name, op in [('add', '+'), ('sub', '-'), ('mul', '*'), ('div', '/')]:
        code += (f'{integer} ref{bits}_{name}({integer} a, {integer} b) {{ '
                 f'bits{bits} x = {{.u=a}}, y = {{.u=b}}, z; z.f=x.f {op} y.f; return z.u; }}\n')
    for name, argument in [('si', 'int32_t'), ('ui', 'uint32_t'), ('i64', 'int64_t'), ('u64', 'uint64_t')]:
        code += (f'{integer} ref{bits}_from_{name}({argument} a) {{ '
                 f'bits{bits} z; z.f=({ty})a; return z.u; }}\n')
    return code


def is_nan(value, bits):
    frac = (1 << (23 if bits == 32 else 52)) - 1
    exp = ((1 << (8 if bits == 32 else 11)) - 1) << (23 if bits == 32 else 52)
    return value & exp == exp and value & frac != 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'runtime/softfloat.c')
    parser.add_argument('--samples', type=int, default=100000)
    parser.add_argument('--bits', type=int, choices=[32, 64], action='append')
    parser.add_argument('--math', action='store_true', help='Also check floor, sqrt and large-angle reduction')
    args = parser.parse_args()
    if args.samples < 0:
        parser.error('samples must be nonnegative')
    randomizer = random.Random(0x50F7F10A7)
    failures = 0
    with tempfile.TemporaryDirectory(prefix='softfloat-oracle-') as directory:
        path = Path(directory)
        code = adapter(args.source.read_text()) + reference(32) + reference(64)
        if args.math:
            math_source = (ROOT / 'runtime/softmath.c').read_text()
            check_pi_table(math_source)
            code += adapter(math_source)
            code += '''#include <math.h>
            uint64_t ref64_sqrt(uint64_t a) { bits64 x={.u=a}, z; z.f=sqrt(x.f); return z.u; }
            uint64_t ref64_floor(uint64_t a) { bits64 x={.u=a}, z; z.f=floor(x.f); return z.u; }
            '''
        code += '''uint32_t ref32_from_f64(uint64_t a) {
            bits64 x = {.u=a}; bits32 z; z.f=(float)x.f; return z.u;
        }
        uint64_t ref64_from_f32(uint32_t a) {
            bits32 x = {.u=a}; bits64 z; z.f=(double)x.f; return z.u;
        }
        '''
        (path / 'oracle.c').write_text(code)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-O2', '-fno-fast-math', '-ffp-contract=off',
                        '-shared', '-fPIC', '-I', str(ROOT / 'runtime'), path / 'oracle.c', '-lm', '-o', path / 'oracle.so'], check=True)
        lib = ctypes.CDLL(str(path / 'oracle.so'))
        for bits in args.bits or [32, 64]:
            uint = ctypes.c_uint32 if bits == 32 else ctypes.c_uint64
            frac_bits, bias = (23, 127) if bits == 32 else (52, 1023)
            sign, one = 1 << (bits - 1), bias << frac_bits
            inf = ((bias * 2 + 1) << frac_bits)
            # Zeros, denormals, normals, carries, ties, extrema and both signs.
            edges = [0, 1, 2, 3, (1 << frac_bits) - 1, 1 << frac_bits,
                     (1 << frac_bits) + 1, one - 1, one, one + 1,
                     one + (1 << (frac_bits - 1)), inf - 1, inf, inf + 1,
                     inf + (1 << (frac_bits - 1)), (bias - frac_bits - 1) << frac_bits]
            edges += [v | sign for v in edges]
            pairs = [(a, b) for a in edges for b in edges]
            pairs += [(randomizer.getrandbits(bits), randomizer.getrandbits(bits))
                      for _ in range(args.samples)]
            # Exercise exact scaling as well as arbitrary significands.
            pairs += [(randomizer.getrandbits(bits), randomizer.randrange(bias * 2 + 1) << frac_bits)
                      for _ in range(args.samples // 10)]
            # Nearby exponents stress cancellation and rounding after alignment.
            for _ in range(args.samples // 10):
                exponent = randomizer.randrange(2 * bias + 1)
                other_exp = max(0, exponent - randomizer.randrange(frac_bits + 4))
                a = (exponent << frac_bits) | randomizer.getrandbits(frac_bits)
                b = (other_exp << frac_bits) | randomizer.getrandbits(frac_bits)
                pairs.append((a, b | (sign if randomizer.getrandbits(1) else 0)))
            for name in ['add', 'sub', 'mul', 'div']:
                actual = getattr(lib, f'__rcc_f{bits}_{name}')
                expected = getattr(lib, f'ref{bits}_{name}')
                actual.argtypes = expected.argtypes = [uint, uint]
                actual.restype = expected.restype = uint
                wrong = 0
                for a, b in pairs:
                    got, want = actual(a, b), expected(a, b)
                    if got != want and not (is_nan(got, bits) and is_nan(want, bits)):
                        if wrong < 3:
                            print(f'f{bits} {name}: {a:x}, {b:x}: got {got:x}, want {want:x}')
                        wrong += 1
                print(f'f{bits} {name}: {len(pairs)} cases, {wrong} failures')
                failures += wrong
            for name, argtype, width, signed in [('si', ctypes.c_int32, 32, True),
                                                ('ui', ctypes.c_uint32, 32, False),
                                                ('i64', ctypes.c_int64, 64, True),
                                                ('u64', ctypes.c_uint64, 64, False)]:
                actual = getattr(lib, f'__rcc_f{bits}_from_{name}')
                expected = getattr(lib, f'ref{bits}_from_{name}')
                actual.argtypes = expected.argtypes = [argtype]
                actual.restype = expected.restype = uint
                values = [0, 1, (1 << (width - int(signed))) - 1]
                values += [v + delta for n in range(width - int(signed))
                           for v in [1 << n] for delta in [-1, 0, 1]]
                if signed:
                    values += [-v for v in values] + [-(1 << (width - 1))]
                values += [argtype(randomizer.getrandbits(width)).value for _ in range(args.samples)]
                wrong = 0
                for value in values:
                    got, want = actual(value), expected(value)
                    if got != want:
                        if wrong < 3:
                            print(f'f{bits} from_{name}: {value}: got {got:x}, want {want:x}')
                        wrong += 1
                print(f'f{bits} from_{name}: {len(values)} cases, {wrong} failures')
                failures += wrong
            other = 64 if bits == 32 else 32
            other_uint = ctypes.c_uint64 if other == 64 else ctypes.c_uint32
            actual = getattr(lib, f'__rcc_f{bits}_from_f{other}')
            expected = getattr(lib, f'ref{bits}_from_f{other}')
            actual.argtypes = expected.argtypes = [other_uint]
            actual.restype = expected.restype = uint
            wrong = 0
            for _ in range(args.samples):
                value = randomizer.getrandbits(other)
                got, want = actual(value), expected(value)
                if got != want and not (is_nan(got, bits) and is_nan(want, bits)):
                    if wrong < 3:
                        print(f'f{bits} from_f{other}: {value:x}: got {got:x}, want {want:x}')
                    wrong += 1
            print(f'f{bits} from_f{other}: {args.samples} cases, {wrong} failures')
            failures += wrong
            # Ripple defines saturating integer conversions, with NaN -> zero.
            values = edges + [randomizer.getrandbits(bits) for _ in range(args.samples)]
            def number(value):
                return struct.unpack('<f' if bits == 32 else '<d',
                                     value.to_bytes(bits // 8, 'little'))[0]
            for name, compare in [('eq', lambda a, b: a == b), ('ne', lambda a, b: a != b),
                                  ('lt', lambda a, b: a < b), ('le', lambda a, b: a <= b),
                                  ('gt', lambda a, b: a > b), ('ge', lambda a, b: a >= b)]:
                actual = getattr(lib, f'__rcc_f{bits}_{name}')
                actual.argtypes, actual.restype = [uint, uint], ctypes.c_int
                wrong = 0
                for a, b in pairs:
                    if actual(a, b) != int(compare(number(a), number(b))):
                        if wrong < 3:
                            print(f'f{bits} {name}: {a:x}, {b:x}')
                        wrong += 1
                print(f'f{bits} {name}: {len(pairs)} cases, {wrong} failures')
                failures += wrong
            for name, argtype, width, signed in [('si', ctypes.c_int32, 32, True),
                                                ('ui', ctypes.c_uint32, 32, False),
                                                ('i64', ctypes.c_int64, 64, True),
                                                ('u64', ctypes.c_uint64, 64, False)]:
                actual = getattr(lib, f'__rcc_f{bits}_to_{name}')
                actual.argtypes, actual.restype = [uint], argtype
                low = -(1 << (width - 1)) if signed else 0
                high = (1 << (width - int(signed))) - 1
                wrong = 0
                for value in values:
                    real = number(value)
                    want = (0 if math.isnan(real) else
                            low if real < low else high if real > high else int(real))
                    got = actual(value)
                    if got != want:
                        if wrong < 3:
                            print(f'f{bits} to_{name}: {value:x}: got {got}, want {want}')
                        wrong += 1
                print(f'f{bits} to_{name}: {len(values)} cases, {wrong} failures')
                failures += wrong
        if args.math:
            values = [randomizer.getrandbits(64) for _ in range(args.samples)]
            values += [0, 1, 2, 3, (1 << 52) - 1, 1 << 52, 0x7FEFFFFFFFFFFFFF,
                       0x7FF0000000000000, 0x7FF8000000000000, 0x8000000000000000]
            for name in ['floor', 'sqrt']:
                actual = getattr(lib, f'__rcc_f64_{name}')
                expected = getattr(lib, f'ref64_{name}')
                actual.argtypes = expected.argtypes = [ctypes.c_uint64]
                actual.restype = expected.restype = ctypes.c_uint64
                wrong = 0
                for value in values:
                    got, want = actual(value), expected(value)
                    if got != want and not (is_nan(got, 64) and is_nan(want, 64)):
                        if wrong < 3:
                            print(f'f64 {name}: {value:x}: got {got:x}, want {want:x}')
                        wrong += 1
                print(f'f64 {name}: {len(values)} cases, {wrong} failures')
                failures += wrong
            reduce = lib.__rcc_f64_reduce_pi2
            reduce.argtypes = [ctypes.c_uint64, ctypes.POINTER(ctypes.c_int32)]
            reduce.restype = ctypes.c_uint64
            quadrant = ctypes.c_int32()
            wrong = tested = 0
            for value in values:
                number = struct.unpack('<d', value.to_bytes(8, 'little'))[0]
                if not math.isfinite(number) or abs(number) < 2**20:
                    continue
                fraction = struct.unpack('<d', reduce(value, ctypes.byref(quadrant)).to_bytes(8, 'little'))[0]
                angle = fraction * (math.pi / 2)
                result = [math.sin(angle), math.cos(angle), -math.sin(angle), -math.cos(angle)][quadrant.value]
                tested += 1
                if abs(fraction) > 0.5 or abs(result - math.sin(number)) > 4e-16:
                    if wrong < 3:
                        print(f'f64 reduce_pi2: {number}: got {result}, want {math.sin(number)}')
                    wrong += 1
            print(f'f64 reduce_pi2: {tested} cases, {wrong} failures')
            failures += wrong
    raise SystemExit(1 if failures else 0)


if __name__ == '__main__':
    main()
