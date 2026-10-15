#!/usr/bin/env python3
"""Compare unchanged bf.rfl with the existing C interpreter on the same program.

python3 rfl/tools/benchmark_bf.py --baseline /tmp/rfl-before --runs 3
C main is replaced by a fixed input runner, excluding interactive prompts. The
matched variant uses 30,000 unsigned 16-bit tape cells like RFL; the stock C
variant keeps its 4096 byte tape. Files and linked binaries are temporary.
"""
import argparse
import json
import re
import tempfile
from pathlib import Path
from benchmark import ROOT, measure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--runs", type=int, default=3)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    source = ROOT / "rfl/examples/bf.rfl"
    program = re.search(r'let program = ("[^"\n]*");', source.read_text())
    if program is None:
        parser.error("cannot find the example's program literal")
    original = (ROOT / "c-test/examples/brainfuck.c").read_text()
    library, count = re.subn(r'int main\(\s*(?:void)?\s*\)\s*\{[\s\S]*', '', original)
    if count != 1:
        parser.error("cannot find the C interpreter's main")
    runner = '\nint main(void) { run(' + program[1] + '); return 0; }\n'
    matched = library.replace('#define TAPE_SIZE 4096', '#define TAPE_SIZE 30000')
    matched = matched.replace('static unsigned char tape', 'static unsigned short tape')
    matched = matched.replace('& 255', '& 65535')
    matched = matched.replace('tape[ptr] = (unsigned char)', 'tape[ptr] = (unsigned short)')
    results = {}
    with tempfile.TemporaryDirectory(prefix="rfl-bf-") as temporary:
        root = Path(temporary)
        c_stock = root / 'stock.c'
        c_stock.write_text(library + runner)
        c_matched = root / 'matched.c'
        c_matched.write_text(matched + runner)
        cases = [
            ('rfl_before', args.baseline, source),
            ('rfl_current', ROOT / 'target/release/rfl', source),
            ('c_stock', ROOT / 'target/release/rcc', c_stock),
            ('c_matched', ROOT / 'target/release/rcc', c_matched),
        ]
        for name, compiler, case in cases:
            if compiler is None:
                continue
            directory = root / name
            directory.mkdir()
            extra = ["-I", ROOT / "runtime/include"] if case.suffix == ".c" else []
            results[name] = measure(compiler.resolve(), case, directory, args.runs, True, extra)
    print(json.dumps(results, indent=2))
    if any(r.get('stdout') != 'Hello World!\n' or r.get('status') != 0 for r in results.values()):
        raise SystemExit('Brainfuck benchmark failed: see results above')


if __name__ == '__main__':
    main()
