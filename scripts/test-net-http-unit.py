#!/usr/bin/env python3
"""Build HTTP parser/client C units privately on native and RVM, without a NIC.

The native single-file rct runner does not link protocol C modules. This runner
links those modules explicitly and checks the same registered rct expectations.
"""
import argparse
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
TESTS = ['test_net_http_parser', 'test_net_http_client', 'test_net_http_request', 'test_net_http_server']


def run(args, mode, test):
    build = args.build_root.resolve() / mode / test
    build.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'c-test/tests/runtime' / (test + '.c')
    sources = [source, ROOT / 'runtime/net/http_parse.c', ROOT / 'runtime/net/http_ascii.c', ROOT / 'runtime/net/http_fields.c', ROOT / 'runtime/string.c']
    if test == 'test_net_http_client':
        sources += [ROOT / 'runtime/net/http.c', ROOT / 'runtime/net/http_request.c']
    if test in ['test_net_http_request', 'test_net_http_server']:
        sources += [ROOT / 'runtime/net/http_request_parse.c']
    if test == 'test_net_http_server':
        sources += [ROOT / 'runtime/net/http_server.c', ROOT / 'runtime/net/http_response.c']
    rcc = str(ROOT / 'target/release/rcc')
    rasm = str(ROOT / 'src/ripple-asm/target/release/rasm')
    rlink = str(ROOT / 'src/ripple-asm/target/release/rlink')
    with (build / 'build.log').open('w') as log:
        def command(argv):
            subprocess.run(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=60)
        objects = []
        if mode == 'rvm':
            sources += [ROOT / 'runtime/putchar.c']
            crt = build / 'crt0.pobj'
            command([rasm, 'assemble', str(ROOT / 'runtime/crt0.asm'), '-o', str(crt), '--bank-size', '16384'])
            objects.append(str(crt))
        for unit in sources:
            if mode == 'native':
                obj = build / (unit.stem + '.ir')
                with obj.open('w') as ir:
                    subprocess.run([rcc, 'compile', str(unit), '--target', 'native', '--emit-ir', '-I', str(ROOT / 'runtime/include')],
                                   cwd=ROOT, stdout=ir, stderr=log, check=True, timeout=60)
            else:
                asm, obj = build / (unit.stem + '.asm'), build / (unit.stem + '.pobj')
                command([rcc, 'compile', str(unit), '-I', str(ROOT / 'runtime/include'), '--bank-size', '16384', '-o', str(asm)])
                command([rasm, 'assemble', str(asm), '--bank-size', '16384', '-o', str(obj)])
            objects.append(str(obj))
        program = build / (test + ('.native' if mode == 'native' else '.bin'))
        if mode == 'native':
            command([rcc, 'link-native', *objects, '-o', str(program)])
            argv = [str(program)]
        else:
            command([rlink, *objects, '-f', 'binary', '--bank-size', '16384', '-o', str(program)])
            argv = [str(ROOT / 'target/release/rvm'), str(program), '--bank-size', '16384', '--memory', '131072',
                    '--headless', '--exit-code', '--timeout', '30', '--disk', str(build / 'unit.disk')]
            if args.jit:
                argv.append('--jit')
        result = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True, timeout=40)
        (build / 'run.log').write_text(result.stdout + result.stderr)
        expected = json.loads(source.with_suffix('.meta.json').read_text())['expected']
        assert result.returncode == 0 and result.stdout == expected, (test, mode, result.stdout, result.stderr)
        print(f'{mode}{"-jit" if mode == "rvm" and args.jit else ""}: {test} passed ({expected.count("Y")} assertions)', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['native', 'rvm'], action='append')
    parser.add_argument('--jit', action='store_true')
    parser.add_argument('--build-root', type=Path, default=ROOT / 'build/network/http-unit')
    args = parser.parse_args()
    for mode in args.mode or ['native', 'rvm']:
        for test in TESTS:
            run(args, mode, test)


if __name__ == '__main__':
    main()
