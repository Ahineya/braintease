#!/usr/bin/env python3
"""Exercise the real clipboard server's bounded transactional protocol."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='nanox-clipboard-') as folder:
    build=Path(folder);source=build/'source'
    subprocess.run([sys.executable,str(ROOT/'ports/nanox/adapt.py'),str(source)],check=True)
    compiler=[os.environ.get('CC','cc')]
    flags=['-fsanitize=undefined']
    if sys.platform=='darwin':
        if 'CC' not in os.environ:compiler=[subprocess.check_output(['xcrun','--find','clang'],text=True).strip()]
        flags+=['-isysroot',os.environ.get('SDKROOT') or subprocess.check_output(['xcrun','--sdk','macosx','--show-sdk-path'],text=True).strip()]
    subprocess.run([*compiler,*flags,
        '-DMWPIXEL_FORMAT=MWPF_TRUECOLOR565','-DMWCONFIG_FILE="nxr_config.h"','-DNXR_DESKTOP=1',
        '-I'+str(ROOT/'ports/nanox/ripple'),'-I'+str(source/'include'),'-I'+str(source/'nanox'),
        str(ROOT/'ports/nanox/tools/clipboard.c'),'-o',str(build/'check')],check=True)
    subprocess.run([str(build/'check')],check=True)
