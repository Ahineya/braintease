#!/usr/bin/env python3
"""Reproduce host engine tests and experimental C16 guest integration gates.

The guest executable is an engine test harness, not an installed interpreter
or a supported embedding SDK. See README.md for integration gates.
All guest tests use disposable images, never the user's installed disks.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from adapt import prepare
from vendor import PORT, verify

ROOT = PORT.parents[1]
CORE = ('quickjs', 'dtoa', 'libregexp', 'libunicode', 'cutils')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, log=None, **kwargs):
    if log is None:
        return subprocess.run(list(map(str, command)), check=True, **kwargs)
    with log.open('w') as output:
        return subprocess.run(list(map(str, command)), check=True, stdout=output,
                              stderr=subprocess.STDOUT, **kwargs)


def host(build, cc):
    binary = build / 'host-reference'
    flags = ['-std=gnu11', '-O2', '-fwrapv', '-D_GNU_SOURCE',
             '-DCONFIG_VERSION="2026-06-04"', '-I' + str(PORT / 'upstream')]
    if sys.platform == 'darwin':
        sdk = os.environ.get('SDKROOT') or subprocess.check_output(
            ['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
        flags += ['-isysroot', sdk]
    command = [*shlex.split(cc), *flags, PORT / 'tests/embed.c',
               *[PORT / 'upstream' / (name + '.c') for name in CORE], '-lm', '-o', binary]
    run(command, build / 'host-build.log', timeout=180)
    start = time.monotonic()
    result = subprocess.check_output([str(binary), str(PORT / 'tests/semantics.js')],
                                     text=True, timeout=30)
    (build / 'host-reference.log').write_text(result)
    print(result.strip(), flush=True)
    # Verify that the harness cannot report success after either an immediate
    # assertion failure or a rejected Promise containing an assertion failure.
    negative_cases = {
        'assertion': "nativeAssert(false, 'expected negative control')",
        'promise': "globalThis.asyncDone=true; Promise.resolve().then(() => nativeAssert(false, 'expected async failure'))",
        'callback': 'nativeAdd()',
    }
    for name, source in negative_cases.items():
        fixture = build / ('negative-' + name + '.js')
        fixture.write_text(source + '\n')
        negative = subprocess.run([str(binary), str(fixture)], capture_output=True, timeout=30)
        (build / ('negative-' + name + '.log')).write_bytes(negative.stdout + negative.stderr)
        if negative.returncode != 1 or b'QUICKJS_REFERENCE' in negative.stdout:
            raise RuntimeError('Host negative control failed: ' + name)
    return dict(status='passed', scope='Full unmodified engine on the host only',
                negative_controls=len(negative_cases),
                elapsed_seconds=time.monotonic() - start, output=result.strip())


def host_numbers(build, cc):
    binary = build / 'numbers-host'
    flags = ['-std=gnu11', '-O2', '-fwrapv', '-I' + str(PORT / 'upstream')]
    if sys.platform == 'darwin':
        sdk = os.environ.get('SDKROOT') or subprocess.check_output(
            ['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
        flags += ['-isysroot', sdk]
    run([*shlex.split(cc), *flags, PORT / 'tests/numbers.c', PORT / 'upstream/dtoa.c',
         PORT / 'upstream/cutils.c', '-lm', '-o', binary], build / 'numbers-host-build.log', timeout=120)
    output = subprocess.check_output([str(binary)], text=True, timeout=30)
    (build / 'numbers-host.log').write_text(output)
    if 'QUICKJS_NUMBERS checks=379 failures=0' not in output:
        raise RuntimeError('Host numeric reference failed')
    print(output.splitlines()[-1], flush=True)
    return dict(status='passed', scope='Unmodified host numeric conversion reference',
                output_records_sha256=hashlib.sha256(output.encode()).hexdigest(),
                records=sum(line.startswith(('N:', 'P:')) for line in output.splitlines()))


def host_support(build, cc):
    binary = build / 'support-host'
    flags = ['-std=gnu11', '-O2', '-fwrapv', '-I' + str(PORT / 'upstream')]
    if sys.platform == 'darwin':
        sdk = os.environ.get('SDKROOT') or subprocess.check_output(
            ['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
        flags += ['-isysroot', sdk]
    run([*shlex.split(cc), *flags, PORT / 'tests/support.c',
         *[PORT / 'upstream' / (name + '.c') for name in ('libunicode', 'libregexp', 'cutils')],
         '-o', binary], build / 'support-host-build.log', timeout=120)
    output = subprocess.check_output([str(binary)], text=True, timeout=60)
    (build / 'support-host.log').write_text(output)
    if 'QUICKJS_SUPPORT checks=' not in output or 'failures=0' not in output or 'FAIL ' in output:
        raise RuntimeError('Host Unicode/regexp reference failed')
    print(output.splitlines()[-1], flush=True)
    negative = subprocess.run([str(binary), '--negative-control'], capture_output=True, timeout=10)
    if negative.returncode != 1 or b'FAIL expected negative control' not in negative.stdout:
        raise RuntimeError('Support negative control failed')
    return dict(status='passed', scope='Pristine host Unicode/regexp semantic reference; guest-only OOM repairs',
                negative_controls=1, output=output.strip())


def host_storage(build, cc):
    from storage_fixture import fixture
    source = build / 'storage-host'; source.mkdir(parents=True, exist_ok=True)
    (source / 'storage-routines.inc').write_text(fixture((PORT / 'upstream/quickjs.c').read_text(), False))
    flags = ['-std=gnu11', '-O2', '-fwrapv', '-I' + str(source), '-I' + str(PORT / 'upstream')]
    if sys.platform == 'darwin':
        sdk = os.environ.get('SDKROOT') or subprocess.check_output(
            ['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
        flags += ['-isysroot', sdk]
    binary = build / 'storage-host-reference'
    run([*shlex.split(cc), *flags, PORT / 'tests/storage.c', PORT / 'upstream/cutils.c',
         '-lm', '-o', binary], build / 'storage-host-build.log', timeout=120)
    process = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    output = process.stdout + process.stderr
    (build / 'storage-host.log').write_text(output)
    if process.returncode or 'failures=0' not in output or 'QUICKJS_STORAGE checks=' not in output:
        raise RuntimeError('Host storage reference failed: ' + output)
    negative = subprocess.run([str(binary), '--negative-control'], capture_output=True, timeout=30)
    if negative.returncode != 1 or b'FAIL expected negative control' not in negative.stdout:
        raise RuntimeError('Storage reference negative control failed')
    print(output.splitlines()[-1], flush=True)
    return dict(status='passed', scope='Extracted pristine core string/serialization routines; test allocation and exception plumbing',
                negative_controls=1, output=output.strip())


def compile_gates(build, compiler, target):
    results = {}
    sdk_includes = [ROOT / 'ports/fuzix/ripple/include',
                    ROOT / 'ports/fuzix/upstream/Library/include', ROOT / 'runtime/include', ROOT / 'ports/fuzix/ripple']
    for profile, includes in [('unmodified', sdk_includes),
                              ('format-header', [PORT / 'config', *sdk_includes])]:
        results[profile] = {}
        for name in CORE:
            flags = ['--target', target, '-DRIPPLE_MMU=1', '-DCONFIG_VERSION="2026-06-04"']
            flags += [v for directory in includes for v in ('-I', str(directory))]
            log = build / f'{target}-{name}-{profile}.log'
            try:
                process = subprocess.run([str(compiler), 'compile',
                    str(PORT / 'upstream' / (name + '.c')), *flags, '--emit-ir'],
                    capture_output=True, timeout=90, env=dict(os.environ, RUST_LOG='error'))
            except subprocess.TimeoutExpired as error:
                log.write_bytes((error.stderr or b'') + b'\nCompilation timed out\n')
                results[profile][name] = dict(status='timeout', log=log.name)
                continue
            log.write_bytes(process.stderr)
            if not process.returncode:
                log.with_suffix('.ir').write_bytes(process.stdout)
            results[profile][name] = dict(status='compiled' if not process.returncode else 'blocked',
                exit_code=process.returncode, diagnostic=process.stderr.decode(errors='replace')[-1500:],
                log=log.name)
    return results


def guest_build(build, compiler, target, kind='foundation'):
    if kind in ('eval', 'full'):
        kind = 'runtime'  # Same engine/fixture; evaluation is a separate run gate.
    engine = kind in ('runtime', 'cli')
    source = prepare(build / 'source')
    if kind in ('storage', 'heap', 'runtime', 'cli'):
        from core_probe import prepare_probe
        source = prepare_probe(build / 'source')
        if kind == 'storage':
            from storage_fixture import fixture
            (source / 'storage-routines.inc').write_text(fixture((source / 'quickjs.c').read_text(), True))
        elif kind == 'heap':
            from heap_fixture import fixture
            (source / 'heap-routines.inc').write_text(fixture((source / 'quickjs.c').read_text()))
    stem = {'numbers': 'qjsnumbers', 'foundation': 'qjscheck', 'support': 'qjssupport',
            'platform': 'qjsplatform', 'storage': 'qjsstorage', 'heap': 'qjsheap', 'runtime': 'qjsruntime', 'cli': 'js'}[kind]
    binary = build / (stem + ('.fxe' if target == 'native' else '.rxe'))
    log = build / (target + '-' + kind + '-build.log')
    inputs = [PORT / ('cli.c' if kind == 'cli' else 'tests/' + {'numbers': 'numbers.c', 'support': 'support.c',
                                'platform': 'platform.c', 'storage': 'storage.c', 'heap': 'heap.c', 'runtime': 'runtime.c', 'foundation': 'c16.c'}[kind]),
              source / 'cutils.c']
    if kind == 'numbers': inputs.append(source / 'dtoa.c')
    if kind == 'support': inputs += [source / 'libunicode.c', source / 'libregexp.c']
    if kind in ('platform', 'heap', 'runtime', 'cli'): inputs.append(source / 'qjs-platform.c')
    if kind == 'cli':
        from embed import prepare_http
        prepare_http(source)
        inputs.append(PORT / 'system.c')
    if engine: inputs += [source / (name + '.c') for name in ('quickjs', 'dtoa', 'libregexp', 'libunicode')]
    defines = ['_BSD_SOURCE=1', 'CONFIG_VERSION="2026-06-04"'] if engine else []
    sdk = build / ('sdk-' + target)
    if target == 'native':
        run([sys.executable, ROOT / 'fuse-x/cc.py', *inputs,
             '-I', source, *[v for define in defines for v in ('-D', define)],
             '--compiler', compiler, '--build', sdk, '--float-format', '-o', binary],
            log, env=dict(os.environ, RUST_LOG='error'), timeout=300)
    else:
        make = ['make', '-s', '--no-print-directory', '-C', ROOT / 'ports/fuzix',
                'BUILD=' + str(sdk), 'RCC=' + str(compiler)]
        run([*make, '-j4', 'rfl-sdk', 'float-sdk'], log,
            env=dict(os.environ, RUST_LOG='error'), timeout=600)
        info = json.loads(subprocess.check_output(list(map(str, [*make, 'rfl-sdk-info'])), text=True))
        floats = json.loads(subprocess.check_output(list(map(str, [*make, 'float-sdk-info'])), text=True))
        libraries = [p for p in info['libraries'] if not p.endswith('/libc/format.pobj')]
        libraries += floats['libraries']
        includes = [source, ROOT / 'ports/fuzix/ripple/include',
                    ROOT / 'ports/fuzix/upstream/Library/include', ROOT / 'runtime/include', ROOT / 'ports/fuzix/ripple']
        assembler = ROOT / 'src/ripple-asm/target/release/rasm'
        objects = []
        with log.open('a') as output:
            for path in inputs:
                cache_kind = 'runtime' if engine and path != PORT / 'cli.c' else kind
                ir = build / ('ripple-' + cache_kind + '-' + path.stem + '.ir')
                with ir.open('w') as ir_output:
                    run([compiler, 'compile', path, '--target', 'ripple', '-DRIPPLE_MMU=1',
                         *['-D' + define for define in defines],
                         *[v for directory in includes for v in ('-I', directory)], '--emit-ir'],
                        stdout=ir_output, stderr=output, env=dict(os.environ, RUST_LOG='error'), timeout=90)
                assembly, obj = ir.with_suffix('.asm'), ir.with_suffix('.pobj')
                stamp = ir.with_suffix('.codegen.json')
                key = dict(compiler=sha(compiler), ir=sha(ir), banked_globals=True)
                cached = assembly.exists() and stamp.exists() and json.loads(stamp.read_text()) == key
                if not cached:
                    stamp.unlink(missing_ok=True)
                    try:
                        run([compiler, 'generate-asm', '-i', ir, '--banked-globals', '-o', assembly],
                            stdout=output, stderr=output, timeout=600 if engine else 90)
                    except subprocess.SubprocessError:
                        assembly.unlink(missing_ok=True)
                        raise
                    stamp.write_text(json.dumps(key) + '\n')
                run([assembler, 'assemble', assembly, '-o', obj, '--bank-size', '64000',
                     '--max-immediate', '65535'], stdout=output, stderr=output, timeout=180 if engine else 90)
                objects.append(obj)
            run([assembler.with_name('rlink'), info['entry'], *objects, *libraries,
                 '-f', 'rxe', '--bank-size', '64000', '-o', binary],
                stdout=output, stderr=output, timeout=90)
    return binary


def guest_test(build, target, binary, kind='foundation'):
    sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
    from guest import Guest
    with tempfile.TemporaryDirectory(prefix='quickjs-' + target + '-') as directory:
        guest = Guest(directory, target, {binary.stem: binary},
                      blocks=65536 if kind in ('runtime', 'eval', 'full') else 32768)
        start = time.monotonic()
        try:
            arguments = ' --full' if kind == 'full' else ' --eval' if kind == 'eval' else ''
            output = guest.shell(binary.stem + arguments + '; echo QJS_STATUS:$?',
                                 60 if kind in ('runtime', 'eval', 'full') else 180)
            marker = {'numbers': 'QUICKJS_NUMBERS checks=379 failures=0',
                      'foundation': 'QUICKJS_C16 checks=30 failures=0',
                      'support': 'QUICKJS_SUPPORT checks=',
                      'platform': 'QUICKJS_PLATFORM checks=',
                      'storage': 'QUICKJS_STORAGE checks=',
                      'heap': 'QUICKJS_HEAP checks=',
                      'runtime': 'QUICKJS_RUNTIME checks=',
                      'eval': 'QUICKJS_RUNTIME checks=',
                      'full': 'QUICKJS_RUNTIME checks='}[kind]
            if marker not in output or 'QJS_STATUS:0' not in output or 'FAIL ' in output or 'failures=0' not in output:
                raise RuntimeError('Guest C16 experiment failed: ' + output)
            summary = re.search(r'QUICKJS_(?:NUMBERS|C16|SUPPORT|PLATFORM|STORAGE|HEAP|RUNTIME) checks=(\d+) failures=0', output)
            if not summary: raise RuntimeError('Missing complete guest check summary')
            if kind in ('eval', 'full') and ('RUNTIME: evaluation completed' not in output or int(summary[1]) <= 36):
                raise RuntimeError('Evaluation gate did not execute the complete JavaScript fixture')
            if kind == 'full' and ('RUNTIME: built-ins completed' not in output or int(summary[1]) < 145):
                raise RuntimeError('Full context gate did not execute all built-in vectors')
            marker = summary.group(0)
            print(target + ': ' + marker, flush=True)
            if kind in ('support', 'platform', 'storage', 'heap', 'runtime', 'eval', 'full'):
                negative = guest.shell(binary.stem + arguments + ' --negative-control; echo QJS_NEGATIVE:$?',
                                       60 if kind in ('eval', 'full') else 20)
                if 'FAIL expected negative control' not in negative or 'QJS_NEGATIVE:1' not in negative:
                    raise RuntimeError('Guest ' + kind + ' negative control failed: ' + negative)
                if kind in ('eval', 'full') and 'FAIL guest evaluation result' not in negative:
                    raise RuntimeError('Evaluation negative control did not detect the deliberately wrong JS result')
            if kind in ('numbers', 'support', 'storage'):
                prefixes = {'numbers': ('N:', 'P:'), 'support': ('U:', 'R:'), 'storage': ('S:', 'B:', 'C:')}[kind]
                records = lambda text: [line for line in text.splitlines() if line.startswith(prefixes)]
                expected = records((build / (kind + '-host.log')).read_text())
                actual = records(output)
                if actual != expected:
                    from itertools import zip_longest
                    mismatch = next((a, e) for a, e in zip_longest(actual, expected) if a != e)
                    raise RuntimeError(f'Guest {kind} output differs from upstream host: {mismatch}')
        except AssertionError:
            # A development probe can loop before the command returns. Deliver
            # guest Ctrl-C so the disposable system can still shut down and fsck
            # normally; preserve and rethrow the original test failure.
            if guest.process.poll() is None:
                try:
                    os.write(guest.master, b'\x03')
                    guest.read(guest.prompt, 10)
                except (AssertionError, OSError):
                    pass
            raise
        finally:
            (build / (target + '-' + kind + '-guest.log')).write_bytes(guest.log)
            guest.close()
    scope = 'C16 numeric conversion; all records match upstream host' if kind == 'numbers' else 'C16 utilities and inline value model only; no JS evaluation'
    if kind == 'support': scope = 'C16 Unicode and regexp; all semantic records match upstream host; no JS evaluation'
    if kind == 'platform': scope = 'C16 embedding ABI, bounded allocator and stack guard; no JS evaluation'
    if kind == 'storage': scope = 'Extracted adapted core string/serialization routines match pristine host; guarded test plumbing; no JS evaluation'
    if kind == 'heap': scope = 'Production QuickJS arena routines and C16 provider; no JSRuntime or JS evaluation'
    if kind == 'runtime': scope = 'Experimental full engine runtime/context, atoms, objects, callbacks, cycle GC and allocator failure unwinding; no JS evaluation'
    if kind == 'eval': scope = 'Real guest JS_Eval with basic intrinsics: focused expression/directive/callback/error/interrupt tests; not full intrinsic initialization or conformance'
    if kind == 'full': scope = 'Full guest context, shared host language vectors, logical-octet typed arrays and pending Promise jobs; experimental, not conformance'
    return dict(status='passed', scope=scope,
                elapsed_seconds=time.monotonic() - start, output=marker,
                checks=int(summary[1]), filesystem_check='passed',
                negative_controls=1 if kind in ('support', 'platform', 'storage', 'heap', 'runtime', 'eval', 'full') else 0,
                executable_sha256=sha(binary), executable_octets=binary.stat().st_size)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/quickjs-explore')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--target', choices=('host', 'native', 'ripple', 'both'), default='host')
    parser.add_argument('--only', choices=('all', 'support', 'platform', 'storage', 'heap', 'runtime', 'eval', 'full'), default='all',
                        help='Rebuild a single guest milestone during port development')
    args = parser.parse_args()
    if args.only in ('platform', 'heap', 'runtime', 'eval', 'full') and args.target == 'host':
        parser.error('The C16 ' + args.only + ' fixture requires --target native, ripple or both')
    verify()
    build = args.build.resolve(); build.mkdir(parents=True, exist_ok=True)
    report = dict(upstream=json.loads((PORT / 'UPSTREAM.json').read_text()),
                  guest_javascript_evaluation=False, targets={})
    if args.only == 'all':
        report.update(host=host(build, args.cc), host_numbers=host_numbers(build, args.cc))
    if args.only in ('all', 'support'): report['host_support'] = host_support(build, args.cc)
    if args.only in ('all', 'storage'): report['host_storage'] = host_storage(build, args.cc)
    compiler = build / 'rcc-pinned'
    if args.target != 'host':
        requested = args.compiler.resolve()
        if requested != compiler and (not compiler.exists() or sha(requested) != sha(compiler)):
            shutil.copy2(requested, compiler)
        report['compiler_sha256'] = sha(compiler)
    report_path = build / ('report.json' if args.only == 'all' else args.only + '-report.json')
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    for target in (('native', 'ripple') if args.target == 'both' else
                   () if args.target == 'host' else (args.target,)):
        report['targets'][target] = {}
        if args.only == 'all': report['targets'][target]['compile_gates'] = compile_gates(build, compiler, target)
        report_path.write_text(json.dumps(report, indent=2) + '\n')
        for kind in ((args.only,) if args.only != 'all' else ('foundation', 'numbers', 'support', 'platform', 'storage', 'heap')):
            try:
                binary = guest_build(build, compiler, target, kind)
                report['targets'][target][kind] = guest_test(build, target, binary, kind)
            except (RuntimeError, AssertionError, subprocess.SubprocessError) as error:
                report['targets'][target][kind] = dict(status='blocked', diagnostic=str(error))
                report_path.write_text(json.dumps(report, indent=2) + '\n')
                raise
            report_path.write_text(json.dumps(report, indent=2) + '\n')
    if args.only in ('eval', 'full'):
        report['guest_javascript_evaluation'] = True
        report_path.write_text(json.dumps(report, indent=2) + '\n')
    print('Experiment report: ' + str(report_path))


if __name__ == '__main__':
    main()
