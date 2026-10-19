#!/usr/bin/env python3
"""Compile and run the portable terminal model checks with bounded execution."""
import os,shlex,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
compiler=shlex.split(os.environ.get('CC','cc'))
flags=['-Wall','-Wextra','-Werror','-fsanitize=undefined']
if sys.platform=='darwin':
    if 'CC' not in os.environ:
        compiler=[subprocess.check_output(['xcrun','--find','clang'],text=True).strip()]
    flags+=['-isysroot',os.environ.get('SDKROOT') or subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()]
with tempfile.TemporaryDirectory(prefix='ripple-vt-') as temp:
    for name in ('terminal','clipboard'):
        binary=Path(temp)/(name+'-test')
        subprocess.run([*compiler,*flags,str(ROOT/f'ports/graphics/tests/{name}.c'),str(ROOT/'ports/graphics/terminal.c'),str(ROOT/'libraries/unicode/unicode.c'),'-o',str(binary)],check=True,timeout=60)
        subprocess.run([str(binary)],check=True,timeout=10)
