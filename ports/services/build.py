#!/usr/bin/env python3
"""Build only the opt-in service tools; never install images or touch boot rules."""
import argparse
from pathlib import Path
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',choices=['host','native','ripple'],default='host')
p.add_argument('--helpers',action='store_true',help='also build synthetic acceptance-test helper')
a=p.parse_args()
out=ROOT/'build/services'/a.target
out.mkdir(parents=True,exist_ok=True)
programs=[('svcd','manager.c'),('svc','client.c'),('console-session','console.c')]
if a.helpers:programs.append(('sv-helper','tests/helper.c'))
for binary,source in programs:
    sources=[HERE/source,HERE/'config.c',ROOT/'libraries/ini/ini.c']
    if a.target=='host':
        cmd=['/usr/bin/clang' if sys.platform=='darwin' else 'cc','-std=c99','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-Wno-parentheses','-I'+str(ROOT/'libraries/ini'),*map(str,sources),'-o',str(out/binary)]
        if sys.platform=='darwin':cmd+=['--ld-path=/Library/Developer/CommandLineTools/usr/bin/ld']
    elif a.target=='native':
        cmd=[sys.executable,str(ROOT/'fuse-x/cc.py'),*map(str,sources),'-I',str(ROOT/'libraries/ini'),'-o',str(out/(binary+'.fxe'))]
    else:
        cmd=['make','-C',str(ROOT/'ports/fuzix'),'-f','Makefile','-f',str(HERE/'ripple.mk'),'BUILD='+str(ROOT/'build/fuzix'),str(out/(binary+'.rxe'))]
    subprocess.run(cmd,cwd=ROOT,check=True)
print('Built service tools in',out,'(not installed or enabled)')
