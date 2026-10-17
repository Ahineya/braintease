#!/usr/bin/env python3
"""Compile representative unmodified Nano-X units with RCC; record blockers."""
import argparse, hashlib, json, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
PORT=ROOT/'ports/nanox'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',type=Path,default=ROOT/'target/release/rcc')
p.add_argument('--build',type=Path,default=ROOT/'build/nanox-audit')
p.add_argument('--unit',action='append',help='Probe only this src-relative unit (repeatable)')
a=p.parse_args();a.build.mkdir(parents=True,exist_ok=True)
units=a.unit or ['engine/devclip.c','engine/devrgn.c','drivers/fblin16.c','drivers/kbd_null.c','nanox/srvmain.c','demos/nanox/nxterm.c','demos/nanox/nxroach.c']
flags=['-DRIPPLE_MMU=1','-DNO_64BIT','-DUNIX=1','-DMWPIXEL_FORMAT=MWPF_TRUECOLOR565','-DMW_CPU_LITTLE_ENDIAN=1','-DUSE_ALLOCA=0','-DHAVE_MMAP=0','-DHAVE_SHAREDMEM_SUPPORT=0','-DHAVE_FREETYPE_2_SUPPORT=0','-DHAVE_HARFBUZZ_SUPPORT=0','-DHAVE_PNG_SUPPORT=0','-DHAVE_JPEG_SUPPORT=0']
includes=[ROOT/'ports/fuzix/ripple/include',ROOT/'ports/fuzix/upstream/Library/include',ROOT/'runtime/include',PORT/'upstream/src/include',PORT/'upstream/src/nanox']
rows=[]
for unit in units:
 source=PORT/'upstream/src'/unit;stem=unit.replace('/','_')
 cmd=[str(a.compiler.resolve()),'compile',str(source),'--bank-size','64000',*flags,*['-I'+str(i) for i in includes],'-o',str((a.build/(stem+'.asm')).resolve())]
 r=subprocess.run(cmd,capture_output=True,text=True)
 log=r.stdout+r.stderr;(a.build/(stem+'.log')).write_text(log)
 rows.append({'source':unit,'command':cmd,'compiled':r.returncode==0,'diagnostic':log[-6000:]})
 print(('PASS ' if r.returncode==0 else 'BLOCKED ')+unit,flush=True)
report={'upstream_revision':'2bf6c0ad1f08af0f0eb6f391d0c1e1d7b7848cab','compiler_sha256':hashlib.sha256(a.compiler.read_bytes()).hexdigest(),'flags':flags,'units':rows,'meaning':'Compile probes only; no link, ABI correctness or integrated graphics claim.'}
(a.build/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print('Report:',a.build/'report.json')
