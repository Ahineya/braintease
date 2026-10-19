#!/usr/bin/env python3
"""Standalone macOS RFL platform, using the private native ABI and host adapters."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]


def run(args,**kwargs):
    result=subprocess.run(list(map(str,args)),capture_output=True,**kwargs)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace')+result.stdout.decode(errors='replace'))
    return result.stdout


def build_host(source,output,rfl,rcc,build=None):
    if sys.platform!='darwin':raise RuntimeError('--os macos requires a macOS build host')
    build=(build or ROOT/'build/rfl-host').resolve()
    build.mkdir(parents=True,exist_ok=True)
    output.parent.mkdir(parents=True,exist_ok=True)
    # Cargo caches the feature build; retain an explicit archive so another
    # ordinary runtime build cannot change which platform this link selects.
    print('Build: Preparing macOS runtime',flush=True)
    run(['cargo','build','--release','-p','rcc-native-runtime','--features','host-tui'],cwd=ROOT)
    runtime=build/'librcc_native_runtime.a'
    shutil.copy2(ROOT/'target/release/librcc_native_runtime.a',runtime)
    with tempfile.TemporaryDirectory(prefix='.rfl-host-',dir=output.parent) as temporary:
        work=Path(temporary)
        print('Build: Compiling RFL and portable libraries',flush=True)
        program=work/'program.ir'
        program.write_bytes(run([rfl,'compile',source,'--target','native','--emit-ir']))
        text=program.read_text()
        sources=[ROOT/'libraries/unicode/unicode.c']
        if '@rfl_curses_' in text:
            sources.append(ROOT/'rfl/host/cells.c')
        if '@rfl_text_paint' in text or '@rfl_text_input' in text:
            sources.append(ROOT/'rfl/host/text.c')
        units=[program]
        for i,path in enumerate(sources):
            ir=work/f'bridge-{i}.ir'
            ir.write_bytes(run([rcc,'compile',path,'--target','native','--bank-size','64000','--emit-ir',
                                '-I',ROOT/'runtime/include']))
            units.append(ir)
        artifact=work/output.name
        print('Build: Linking macOS executable',flush=True)
        libraries=[name for name in os.environ.get('RCC_NATIVE_LIBS','').split(',') if name]
        if 'ncurses' not in libraries:libraries.append('ncurses')
        env=dict(os.environ,RCC_NATIVE_RUNTIME=str(runtime),RCC_NATIVE_LIBS=','.join(libraries))
        run([rcc,'link-native',*units,'-o',artifact],env=env)
        artifact.replace(output)
    print('Built macOS application: '+str(output),flush=True)
