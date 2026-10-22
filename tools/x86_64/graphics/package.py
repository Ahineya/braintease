#!/usr/bin/env python3
"""Prepare three existing RXE1 artifacts; does not rebuild apps or catalogs."""
import hashlib,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools/packages'))
from catalog import Catalog
build=ROOT/'build/x86_64-graphics';out=build/'catalog';out.mkdir(exist_ok=True)
engine=ROOT/'build/pkg/host/pkg-engine'
if not engine.is_file():raise RuntimeError('Existing host package engine required')
catalog=Catalog(build,out,engine,'x64')
for name in ('lvcalc','screenshot','touchpad'):
    binary=build/'apps'/(name+'.fxe');data=binary.read_bytes()
    if data[:12]!=b'FUSEX001RXE1':raise RuntimeError('Expected existing RXE1: '+str(binary))
    if name!='lvcalc':catalog.application(name,binary);continue
    files=[{'name':'usr/bin/lvcalc','mode':0o755,'data':data},
           {'name':'bin/lvcalc','mode':0o755,'data':b'#!/bin/sh\nexec /usr/bin/lvcalc "$@"\n'}]
    notices=ROOT/'build/lvgl/system-dialogs/apps/licenses'
    for notice in sorted(notices.glob('*.txt')):
        files.append({'name':'usr/share/doc/lvcalc/'+notice.name,'mode':0o644,'data':notice.read_bytes()})
    if len(files)!=7:raise RuntimeError('Expected all five LVGL/font/icon notices')
    catalog.package(name,files,['system-licenses@*']);catalog.apps.append(name)
report={'target':'riplix-x64-c16','applications':catalog.apps,'packages':list(catalog.packages.values()),'rebuild':False,'mode':'additive three app identities; existing signed repository provides system-licenses'}
(out/'catalog.json').write_text(json.dumps(report,indent=2)+'\n')
for p in report['packages']:subprocess.run([str(engine),'inspect',p['archive']],check=True)
print(out/'catalog.json')
