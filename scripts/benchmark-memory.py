#!/usr/bin/env python3
"""Compare checked C16 memory workloads using frozen compilers/runtime sources.

Reports complete-program instruction counts (including initialization), binary
and source hashes, and RVM wall times. Optional BF execution uses the same object
files and verifies the same result. No normal guest disk is opened or changed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <string.h>
#include <stdio.h>
static char a[LENGTH+32], b[LENGTH+32];
volatile unsigned long result;
int main(void) {
    unsigned i, r;
    for (i = 0; i < LENGTH+32; i++) { a[i] = 60000U; b[i] = 60000U; }
    a[11+LENGTH] = 0; b[7+LENGTH] = 0;
    for (r = 0; r < ROUNDS; r++) {
#if OP == 0
        memcpy(b+7, a+11, LENGTH);
        result = (unsigned char)b[7];
#elif OP == 1
        memset(b+7, 60000U, LENGTH);
        result = (unsigned char)b[7];
#elif OP == 2
        result = strlen(a+11);
#elif OP == 3
        result = strcmp(a+11, b+7);
#elif OP == 4
        memmove(b+8, b+7, LENGTH);
        result = (unsigned char)b[8];
#endif
    }
#if OP == 2
    if (result != LENGTH) return 1;
#elif OP == 3
    if (result != 0) return 1;
#else
    if (result != 60000U) return 1;
#endif
    putchar('Y');
    return 0;
}
'''


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, timeout=180):
    p = subprocess.run(list(map(str, args)), cwd=ROOT, capture_output=True,
                       text=True, timeout=timeout)
    if p.returncode:
        raise RuntimeError(p.stdout + p.stderr)
    return p.stdout


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler', type=Path, default=ROOT/'target/release/rcc')
    p.add_argument('--runtime-source', type=Path, default=ROOT/'runtime/string.c')
    p.add_argument('--baseline-compiler', type=Path)
    p.add_argument('--baseline-runtime', type=Path)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--bank-size', type=int, default=2048)
    p.add_argument('--length', type=int, default=1200)
    p.add_argument('--rounds', type=int, default=32)
    p.add_argument('--repetitions', type=int, default=3)
    p.add_argument('--bf', action='store_true')
    p.add_argument('--timeout', type=int, default=60)
    a = p.parse_args()
    if not (1 <= a.bank_size <= 65535 and 1 <= a.length <= 60000 and
            1 <= a.rounds <= 65535 and a.repetitions > 0):
        p.error('invalid bank size, length, rounds or repetitions')
    if bool(a.baseline_compiler) != bool(a.baseline_runtime):
        p.error('supply both baseline compiler and runtime source')
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    asm = ROOT/'src/ripple-asm/target/release/rasm'
    link = ROOT/'src/ripple-asm/target/release/rlink'
    bank = str(a.bank_size)
    source = out/'workload.c'; source.write_text(SOURCE)
    disk = out/'empty.img'
    if not disk.exists(): disk.write_bytes(b'')
    if disk.stat().st_size: p.error('benchmark empty.img must be empty')
    startup = out/'crt.pobj'
    run([asm, 'assemble', ROOT/'runtime/crt0.asm', '-o', startup, '--bank-size', bank])
    variants = [('after', a.compiler, a.runtime_source)]
    if a.baseline_compiler:
        variants.insert(0, ('before', a.baseline_compiler, a.baseline_runtime))
    records = {}
    for label, compiler, runtime in variants:
        compiler, runtime = compiler.resolve(), runtime.resolve()
        directory = out/label; directory.mkdir(exist_ok=True)
        records[label] = dict(compiler_sha256=digest(compiler), runtime_sha256=digest(runtime), cases={})
        (directory/'string-source.c').write_bytes(runtime.read_bytes())
        if '__banked_memory.h' in runtime.read_text():
            header = ROOT/'runtime/include/__banked_memory.h'
            (directory/'__banked_memory.h').write_bytes(header.read_bytes())
            records[label]['banked_header_sha256'] = digest(header)
        def compile_object(path, stem, defines=()):
            assembly, obj = directory/(stem+'.asm'), directory/(stem+'.pobj')
            run([compiler, 'compile', path, '-o', assembly, '-I'+str(ROOT/'runtime/include'),
                 '--bank-size', bank, '-DBANK_SIZE='+bank, *defines])
            run([asm, 'assemble', assembly, '-o', obj, '--bank-size', bank])
            return obj
        library = compile_object(runtime, 'string')
        putchar = compile_object(ROOT/'runtime/putchar.c', 'putchar')
        for op, name in enumerate(['memcpy', 'memset', 'strlen', 'strcmp', 'memmove']):
            obj = compile_object(source, name, [f'-DOP={op}', f'-DLENGTH={a.length}', f'-DROUNDS={a.rounds}'])
            objects = [startup, obj, library, putchar]
            binary = directory/(name+'.bin')
            run([link, *objects, '-o', binary, '-f', 'binary', '--bank-size', bank])
            # Globals plus stack; reserve at least eight RAM banks.
            banks = max(8, (2*(a.length+32)+a.bank_size-1)//a.bank_size+5)
            times, instructions = [], []
            for repetition in range(a.repetitions):
                report = directory/f'{name}-{repetition}.json'
                start = time.monotonic()
                stdout = run([ROOT/'target/release/rvm', binary, '--headless', '--net', 'off',
                    '--disk', disk, '--bank-size', bank, '--memory', str(banks*a.bank_size),
                    '--exit-code', '--timeout', str(a.timeout), '--report', report], a.timeout+10)
                times.append(time.monotonic()-start)
                if stdout != 'Y': raise RuntimeError(f'{label}/{name}: {stdout!r}')
                instructions.append(json.loads(report.read_text())['instructions'])
            if len(set(instructions)) != 1: raise RuntimeError('nondeterministic instruction counts')
            case = dict(instructions=instructions[0], rvm_seconds=times,
                        median_seconds=statistics.median(times), binary_sha256=digest(binary))
            if a.bf:
                macro, bf = directory/(name+'.bfm'), directory/(name+'.bf')
                run([link, *objects, '-o', macro, '-f', 'macro', '--standalone', '--bf-host',
                     '--memory-banks', str(banks), '--bank-size', bank])
                run([ROOT/'src/bf-macro-expander/target/release/bfm', 'expand', macro,
                     '--strip-comments', '-o', bf])
                report = directory/(name+'-bf.json')
                stdout = run([ROOT/'target/release/rbf', bf, '--headless', '--net', 'off',
                    '--exit-code', '--timeout', str(a.timeout), '--report', report], a.timeout+10)
                if stdout != 'Y': raise RuntimeError(f'{label}/{name} BF: {stdout!r}')
                case['bf_report'] = json.loads(report.read_text())
                case['bf_sha256'] = digest(bf)
            records[label]['cases'][name] = case
            print(label, name, instructions[0], flush=True)
    result = dict(platform=platform.platform(), bank_size=a.bank_size, length=a.length,
                  rounds=a.rounds, source_sha256=digest(source), variants=records,
                  tool_sha256={str(t.relative_to(ROOT)): digest(t) for t in
                    [asm, link, ROOT/'target/release/rvm', *([ROOT/'target/release/rbf',
                    ROOT/'src/bf-macro-expander/target/release/bfm'] if a.bf else [])]})
    (out/'results.json').write_text(json.dumps(result, indent=2)+'\n')


if __name__ == '__main__':
    main()
