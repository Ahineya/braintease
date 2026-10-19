#!/usr/bin/env python3
"""Build NetSurf acceptance programs or the Nano-X graphical browser."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import fcntl
import json
from pathlib import Path
import subprocess
import sys
from build import ROOT, PORT, build, includes
from adapt import write_changed


def application(args):
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    directory = args.build.resolve()
    compiler = args.compiler.resolve()
    objects = build(directory, compiler, args.target, args.jobs)
    source = args.source.resolve()
    extra_includes = includes(directory / 'source')
    if args.core:
        from core import build_core, core_includes
        objects = [p for p in objects if p.stem != 'runtime_softfloat']
        objects += build_core(directory, compiler, args.target, args.jobs)
        extra_includes = core_includes(directory / 'source')
    if args.web:
        # Independent generic HTTP/TLS executable; NetSurf never links wolfSSL.
        helper = output.parent / ('htget.fxe' if args.target == 'native' else 'htget.rxe')
        command = [sys.executable, str(ROOT/'ports/tls/app.py'), '--target', args.target,
                   '--compiler', str(compiler), '--tls-build', str(args.tls_build.resolve()),
                   '--build-dir', str((args.build/('sdk-worker-'+args.target)).resolve()),
                   '-j', str(args.jobs), '-o', str(helper)]
        subprocess.run(command, check=True)
        web_ir=directory/('core-'+args.target)/'c16_http.ir'
        flags=['--target',args.target,'-DRIPPLE_MMU=1']
        flags += [v for path in extra_includes for v in ('-I',str(path))]
        result=subprocess.run([str(compiler),'compile',str(PORT/'http/fetch.c'),*flags,'--emit-ir'],capture_output=True)
        web_ir.with_suffix('.log').write_bytes(result.stderr)
        if result.returncode: raise RuntimeError('HTTP adapter: see '+str(web_ir.with_suffix('.log')))
        write_changed(web_ir,result.stdout)
        objects.append(web_ir)
    frontend_flags = []
    if args.gui:
        from nano import client
        client_objects, client_includes, frontend_flags = client(directory, compiler, args.target, args.jobs)
        objects += client_objects
        extra_includes += client_includes
    if args.images:
        subprocess.run([sys.executable, str(ROOT/'libraries/image/adapt.py')], check=True)
        image_sources=[ROOT/'libraries/image/decode.c',PORT/'image/content.c']
        if args.gui:
            image_sources.append(ROOT/'libraries/image/nano.c')
        flags=['--target',args.target,'-DRIPPLE_MMU=1',*frontend_flags]
        sdk_includes=[ROOT/'ports/fuzix/ripple/include',ROOT/'ports/fuzix/upstream/Library/include',ROOT/'runtime/include']
        flags += [v for path in [*extra_includes,*sdk_includes] for v in ('-I',str(path))]
        for path in image_sources:
            ir=directory/('core-'+args.target)/('image_'+path.stem+'.ir')
            result=subprocess.run([str(compiler),'compile',str(path),*flags,'--emit-ir'],capture_output=True)
            ir.with_suffix('.log').write_bytes(result.stderr+(result.stdout if result.returncode else b''))
            if result.returncode: raise RuntimeError('Image library: see '+str(ir.with_suffix('.log')))
            write_changed(ir,result.stdout);objects.append(ir)
    if args.target == 'native':
        command = [sys.executable, str(ROOT / 'fuse-x/cc.py'), str(source),
                   '--compiler', str(compiler), '--build', str(args.sdk.resolve()), '-o', str(output)]
        command += ['-I' + str(path) for path in extra_includes]
        if args.core:
            command += ['--float-format']
        command += frontend_flags
        for ir in objects:
            command += ['--object-ir', str(ir)]
        subprocess.run(command, check=True)
    else:
        sdk_command = ['make', '-s', '--no-print-directory', '-C', str(ROOT / 'ports/fuzix'),
                       'BUILD=' + str(args.sdk.resolve()), 'RCC=' + str(compiler)]
        if not args.skip_sdk:
            subprocess.run([*sdk_command, '-j' + str(args.jobs), 'rfl-sdk',
                            *(['float-sdk'] if args.core else [])], check=True)
        info = json.loads(subprocess.check_output([*sdk_command, 'rfl-sdk-info'], text=True))
        if args.core:
            floats = json.loads(subprocess.check_output([*sdk_command, 'float-sdk-info'], text=True))
            info['libraries'] = [p for p in info['libraries'] if not p.endswith('/libc/format.pobj')]
            info['libraries'] += floats['libraries']
        ir = directory / 'ripple' / (output.stem + '.ir')
        flags = ['-DRIPPLE_MMU=1', *([] if args.core else ['-DNO_64BIT']), *frontend_flags]
        for path in extra_includes:
            flags += ['-I', str(path)]
        result = subprocess.run([str(compiler), 'compile', str(source), *flags, '--emit-ir'], capture_output=True)
        ir.with_suffix('.log').write_bytes(result.stderr + (result.stdout if result.returncode else b''))
        if result.returncode:
            raise RuntimeError(f'{source}: see {ir.with_suffix(".log")}')
        write_changed(ir, result.stdout)
        assembler = ROOT / 'src/ripple-asm/target/release/rasm'
        def lower(path):
            assembly, obj = path.with_suffix('.asm'), path.with_suffix('.pobj')
            if obj.exists() and obj.stat().st_mtime_ns >= max(p.stat().st_mtime_ns for p in (path, compiler, assembler)):
                return obj
            with path.with_suffix('.lower.log').open('w') as log:
                subprocess.run([str(compiler), 'generate-asm', '-i', str(path), '--banked-globals',
                                '-o', str(assembly)], stdout=log, stderr=log, check=True)
                subprocess.run([str(assembler), 'assemble', str(assembly), '-o', str(obj),
                                '--bank-size', '64000', '--max-immediate', '65535'],
                               stdout=log, stderr=log, check=True)
            return obj
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            inputs = list(pool.map(lower, [ir, *objects]))
        with output.with_suffix('.link.log').open('w') as log:
            subprocess.run([str(assembler.with_name('rlink')), info['entry'], *map(str, inputs),
                            *info['libraries'], '-f', 'rxe', '--bank-size', '64000', '-o', str(output)],
                           stdout=log, stderr=log, check=True)
    print(f'Built {output}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/netsurf')
    parser.add_argument('--target', choices=('native', 'ripple'), default='native')
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--source', type=Path)
    parser.add_argument('--skip-sdk', action='store_true')
    parser.add_argument('--core', action='store_true', help='Link the local-page core and floating-point SDK')
    parser.add_argument('--web',action='store_true',help='Enable the guest HTTP worker (implies --core)')
    parser.add_argument('--tls-build',type=Path,default=ROOT/'build/tls')
    parser.add_argument('--images', action='store_true', help='Link the shared image decoders (implies --core)')
    parser.add_argument('--gui', action='store_true', help='Build the Nano-X browser and HTTP worker (implies --web and --core)')
    parser.add_argument('-j', '--jobs', type=int, default=3)
    parser.add_argument('-o', '--output', type=Path, required=True)
    args = parser.parse_args()
    args.web = args.web or args.gui
    args.images = args.images or args.gui
    args.core = args.core or args.web or args.images
    if args.source is None:
        args.source = PORT / ('frontend/nano.c' if args.gui else 'tests/core.c' if args.core else 'tests/dependencies.c')
    if args.sdk is None:
        args.sdk = args.build / (('sdk-native' if args.target == 'native' else 'sdk-ripple') + ('-float' if args.core else ''))
    args.build.mkdir(parents=True, exist_ok=True)
    with (args.build / (args.target + '.lock')).open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        application(args)


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(f'NetSurf build command failed (status {error.returncode}); see diagnostics above.', file=sys.stderr)
        sys.exit(1)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
