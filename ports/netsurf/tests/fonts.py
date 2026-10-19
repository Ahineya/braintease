#!/usr/bin/env python3
"""Build/run portable font metrics and guarded real-atlas tests on the host."""
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[3]
OUT=ROOT/'build/netsurf/fonts';OUT.mkdir(parents=True,exist_ok=True)
flags=['cc','-std=c99','-O2']
for directory in ('ports/netsurf/upstream/netsurf','ports/netsurf/upstream/netsurf/include',
                  'ports/netsurf/upstream/libwapcaplet/include','ports/nanox/upstream/src/include',
                  'ports/nanox/ripple','ports/fusede/include'):
    flags+=['-I',str(ROOT/directory)]
flags+=['-DMWCONFIG_FILE="nxr_config.h"']
for name in ('fonts','fonts_paint'):
    program=OUT/name
    subprocess.run([*flags,str(ROOT/'ports/netsurf/tests'/(name+'.c')),
                    str(ROOT/'libraries/unicode/unicode.c'),str(ROOT/'libraries/unicode/font.c'),
                    '-o',str(program)],check=True)
    args=[str(ROOT/'ports/netsurf/fonts/web.nfa')]
    if name=='fonts':args+=[str(OUT/'bad.nfa')]
    subprocess.run([str(program),*args],check=True,timeout=120)
