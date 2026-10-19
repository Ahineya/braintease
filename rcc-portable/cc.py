#!/usr/bin/env python3
"""Host-assisted C16 compilation through the portable frontend and RCC IR."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


class MacroOption(argparse.Action):
    def __call__(self, parser, namespace, value, option_string=None):
        # -D/-U must be applied in command-line order.
        options = list(getattr(namespace, 'macro_options', None) or [])
        options += [option_string, value]
        namespace.macro_options = options


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('-o', '--output', type=Path)
    parser.add_argument('-I', dest='includes', action='append', default=[])
    parser.add_argument('-D', action=MacroOption)
    parser.add_argument('-U', action=MacroOption)
    parser.set_defaults(macro_options=[])
    parser.add_argument('--target', choices=['ripple', 'native'], default='ripple')
    parser.add_argument('--backend',choices=['rust','portable'],default='rust')
    parser.add_argument('--emit-ir', action='store_true')
    parser.add_argument('--preprocessed', action='store_true')
    parser.add_argument('--rcc', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--rcpp', type=Path, help='use a host preprocessor instead of the portable one')
    args = parser.parse_args()
    if args.backend=='portable' and args.target!='ripple':
        parser.error('the portable backend currently generates Ripple assembly only')
    frontend = HERE / 'build/rcc-c'
    inputs = list((HERE / 'src').glob('*.c')) + list((HERE / 'upstream/cproc').glob('*.[ch]'))
    inputs += [HERE / 'adapt.py', HERE / 'build.py', HERE / 'preprocessor.c']
    if not frontend.exists() or any(p.stat().st_mtime_ns > frontend.stat().st_mtime_ns for p in inputs):
        subprocess.run([sys.executable, str(HERE / 'build.py')], check=True, stdout=sys.stderr)
    with tempfile.TemporaryDirectory(prefix='rcc-portable-') as temp:
        directory = Path(temp)
        source = args.input.resolve()
        options = []
        if not args.preprocessed and args.rcpp:
            prepared = directory / 'input.i'
            command = [str(args.rcpp.resolve()), str(source), '--keep-line-directives', '-I', str(source.parent),
                       '-D__STDC_VERSION__=201112L', '-D__RCC_PORTABLE__=1']
            for include in args.includes:
                command += ['-I', include]
            command += args.macro_options
            command += ['-I', str(ROOT / 'runtime/include')]
            result = subprocess.run(command, check=True, capture_output=True)
            prepared.write_bytes(('#line 1 ' + json.dumps(str(args.input.resolve())) + '\n').encode() + result.stdout)
            source = prepared
        elif not args.preprocessed:
            for include in [*args.includes, str(ROOT / 'runtime/include')]:
                options += ['-I', include]
            options += args.macro_options
        ir = directory / 'module.ir'
        # An unsuccessful frontend never publishes its partial output.
        subprocess.run([str(frontend), *options, '--target', args.target, str(source), '-o', str(ir)], check=True)
        if args.emit_ir:
            if args.output:
                with tempfile.NamedTemporaryFile(dir=args.output.resolve().parent, delete=False) as file:
                    temporary = Path(file.name)
                    file.write(ir.read_bytes())
                try:
                    os.replace(temporary, args.output)
                finally:
                    temporary.unlink(missing_ok=True)
            else:
                sys.stdout.buffer.write(ir.read_bytes())
        else:
            if not args.output:
                parser.error('-o is required when generating code')
            if args.target == 'ripple':
                if args.backend=='portable':
                    from build_ripple import ensure_host
                    command=[str(ensure_host()),'--unit',str(args.input.resolve()),str(ir),'-o',str(args.output)]
                else:
                    command = [str(args.rcc.resolve()), 'generate-asm', '-i', str(ir), '-o', str(args.output)]
            else:
                command = [str(args.rcc.resolve()), 'link-native', str(ir), '-o', str(args.output)]
            subprocess.run(command, check=True)


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        if error.stderr:
            sys.stderr.buffer.write(error.stderr)
        sys.exit(error.returncode)
    except OSError as error:
        print(f'portable compiler: {error}', file=sys.stderr)
        sys.exit(1)
