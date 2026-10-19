#!/usr/bin/env python3
"""Build the same HTTPS chat client for both C16 guest ABIs."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import fcntl
import json
from pathlib import Path
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'ports/tls'))
from build import build as tls_build


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target', choices=('native', 'ripple'), default='native')
    p.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    p.add_argument('--build-dir', type=Path)
    p.add_argument('--skip-sdk', action='store_true')
    p.add_argument('-j', type=int, default=3)
    p.add_argument('-o', '--output', type=Path, required=True)
    a = p.parse_args()
    compiler, output = a.compiler.resolve(), a.output.resolve()
    sdk = (a.build_dir or ROOT / ('fuse-x/build' if a.target == 'native' else 'build/fuzix')).resolve()
    tls = sdk / 'tls'
    tls.mkdir(parents=True, exist_ok=True)
    with (tls / (a.target + '.lock')).open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        inputs = tls_build(tls, compiler, a.target, a.j)
    includes = [ROOT / 'ports/tls', ROOT / 'ports/fuzix/ripple/include', ROOT / 'ports/fuzix/upstream/Library/include',
                ROOT / 'runtime/include', ROOT / 'ports/fuzix/ripple']
    sources = [PORT / (n + '.c') for n in ('main', 'ui', 'composer', 'mentions', 'json', 'store', 'http', 'network', 'tools', 'tool_history')]
    output.parent.mkdir(parents=True, exist_ok=True)
    # Atomic final installation; a failed compile keeps the last working app.
    artifact = output.with_name('.' + output.name + '.new')
    if a.target == 'native':
        command = [sys.executable, str(ROOT / 'fuse-x/cc.py'), *map(str, sources), '--curses',
                   '--compiler', str(compiler), '--build', str(sdk), '-o', str(artifact)]
        command += ['-I' + str(d) for d in includes]
        for ir in inputs:
            command += ['--object-ir', str(ir)]
        subprocess.run(command, check=True)
    else:
        make = ['make', '-s', '--no-print-directory', '-C', str(ROOT / 'ports/fuzix'), f'BUILD={sdk}',
                f'RCC={compiler}', 'RFL_CURSES=1', 'RFL_UNICODE=1']
        if not a.skip_sdk:
            subprocess.run([*make, f'-j{a.j}', 'rfl-sdk'], check=True)
        info = json.loads(subprocess.check_output([*make, 'rfl-sdk-info'], text=True))
        work = sdk / 'chat'
        work.mkdir(parents=True, exist_ok=True)
        for source in sources:
            ir = work / (source.stem + '.ir')
            result = subprocess.run([str(compiler), 'compile', str(source), '--emit-ir', '-DRIPPLE_MMU=1', '-DNO_64BIT',
                                     *['-I' + str(d) for d in includes]], capture_output=True)
            ir.with_suffix('.log').write_bytes(result.stderr)
            if result.returncode:
                raise RuntimeError(result.stderr.decode(errors='replace'))
            if not ir.exists() or ir.read_bytes() != result.stdout:
                ir.write_bytes(result.stdout)
            inputs.append(ir)
        assembler = ROOT / 'src/ripple-asm/target/release/rasm'
        def lower(ir):
            assembly, obj = ir.with_suffix('.asm'), ir.with_suffix('.pobj')
            if obj.exists() and obj.stat().st_mtime_ns >= max(f.stat().st_mtime_ns for f in (ir, compiler, assembler)):
                return obj
            with ir.with_suffix('.lower.log').open('w') as log:
                subprocess.run([str(compiler), 'generate-asm', '-i', str(ir), '--banked-globals', '-o', str(assembly)], stdout=log, stderr=log, check=True)
                subprocess.run([str(assembler), 'assemble', str(assembly), '-o', str(obj), '--bank-size', '64000', '--max-immediate', '65535'], stdout=log, stderr=log, check=True)
            return obj
        with ThreadPoolExecutor(max_workers=a.j) as pool:
            objects = list(pool.map(lower, inputs))
        with output.with_suffix('.link.log').open('w') as log:
            subprocess.run([str(assembler.with_name('rlink')), info['entry'], *map(str, objects), *info['libraries'],
                            '-f', 'rxe', '--bank-size', '64000', '-o', str(artifact)], stdout=log, stderr=log, check=True)
    artifact.replace(output)
    print(f'Built chat ({a.target}): {output}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as e:
        sys.exit(str(e))
