#!/usr/bin/env python3
"""Link the existing adapted LVGL calculator for bare-metal C16/RXE1.

Consumes an existing LVGL adaptation and its IR cache read-only. Missing object
IR is compiled in this build directory; neither LVGL source nor its build cache
is changed. Uses the same SDK/compiler and UI sources as the existing port.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[3]
PORT=ROOT/'ports/lvgl'
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,default=ROOT/'build/x86_64-graphics')
    p.add_argument('--lvgl-base',type=Path,default=ROOT/'build/lvgl/system-dialogs/apps')
    p.add_argument('--compiler',type=Path,default=ROOT/'target/release/rcc')
    p.add_argument('--desktop',action='store_true',help='build the six Graphite apps and graphical terminal for the native desktop')
    p.add_argument('-j','--jobs',type=int,default=4)
    p.add_argument('--screenshot-only',action='store_true')
    a=p.parse_args();build=a.build.resolve();base=a.lvgl_base.resolve()
    build.mkdir(parents=True,exist_ok=True);apps=build/'apps';apps.mkdir(exist_ok=True)
    # Copy tiny cached SDK IR files; new fingerprints/outputs stay isolated.
    sdk=build/'sdk';sdk.mkdir(exist_ok=True)
    seed=ROOT/'build/x86_64-tui-wave/sdk'
    if seed.exists():
        for path in seed.glob('*.ir'):
            destination=sdk/path.name
            if not destination.exists():shutil.copyfile(path,destination)
    env=dict(os.environ,RCC_NATIVE_MEMORY='checked')
    cc=[sys.executable,str(ROOT/'fuse-x/cc.py')]
    compiler=a.compiler.resolve()
    common=['--static','--heap-profile','bounded','--linker',str(ROOT/'tools/x86_64/app_link.py'),'--build',str(build),'--compiler',str(compiler)]
    if a.desktop:common+=['-D','GX_HOSTED_MAX_HEIGHT=1200']
    subprocess.run([*cc,str(Path(__file__).with_name('screenshot.c')),*common,'-o',str(apps/'screenshot.fxe')],check=True,env=env)
    if a.screenshot_only:return
    if a.desktop:
        subprocess.run([*cc,str(Path(__file__).with_name('desktop-probe.c')),*common,'-o',str(apps/'desktop-probe.fxe')],check=True,env=env)
    source,config=base/'src',base/'config'
    if not (source/'lvgl.h').exists() or not (config/'lv_conf.h').exists():
        raise RuntimeError('Existing LVGL adapted source required; build ports/lvgl once first')
    if a.desktop and not re.search(r'^#define\s+LV_DRAW_SW_SUPPORT_ARGB8888\s+1\b',(config/'lv_conf.h').read_text(),re.M):
        raise RuntimeError('Desktop panels require the current LVGL adaptation with ARGB8888 enabled; refresh the isolated LVGL base with ports/lvgl/adapt.py prepare()')
    includes=[ROOT/'ports/fuzix/ripple/include',ROOT/'ports/fuzix/upstream/Library/include',ROOT/'runtime/include',ROOT/'ports/fuzix/ripple',ROOT/'ports/graphics/include',ROOT/'libraries/text',source,config,PORT]
    flags=['--target','native','--bank-size','64000','-DRIPPLE_MMU=1','-DFUSE_X_USER=1','-DLV_CONF_INCLUDE_SIMPLE=1','-D__func__="C16"']
    flags += [part for path in includes for part in ('-I',str(path))]
    digest=hashlib.sha256(str((compiler.stat().st_mtime_ns,flags)).encode())
    for directory in includes:
        for path in sorted(directory.rglob('*.h')):digest.update(str(path).encode());digest.update(path.read_bytes())
    prefix=digest.digest()
    units=module('hp_lvgl_adapt',PORT/'adapt.py').sources(source)
    units+=module('hp_lvgl_fonts',PORT/'fonts/pack.py').prepare(build)
    units += [PORT/name for name in ('font-data.c','fonts/symbols.c','icons/data.c','ui-scale.c','ui.c','ui-data.c','ui-app.c','ui-forms.c','browser.c','ui-tabs.c','plot.c','plot-raster.c','ui-plot.c')]
    cache=build/'objects';cache.mkdir(exist_ok=True)
    reused=[];compiled=[]
    def one(path):
        key=hashlib.sha256(prefix+path.read_bytes()).hexdigest()[:20]
        name=path.stem+'-'+key+'.ir';old=base/'objects'/name;output=cache/name
        if old.exists():reused.append(str(old));return old
        if not output.exists():
            r=subprocess.run([str(compiler),'compile',str(path),*flags,'--emit-ir'],capture_output=True)
            (cache/(path.stem+'.log')).write_bytes(r.stderr)
            if r.returncode:raise RuntimeError(str(path)+'\n'+r.stderr.decode(errors='replace')[-4000:])
            output.write_bytes(r.stdout);compiled.append(str(path))
        return output
    print('LVGL: resolving',len(units),'existing units',flush=True)
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:objects=list(pool.map(one,units))
    print('LVGL:',len(reused),'cached,',len(compiled),'compiled',flush=True)
    selected=['lvcalc','lvselect','lvgallery','lvdata','lvedit','fusedeshell'] if a.desktop else ['lvcalc']
    app_sources={'lvcalc':['calculator.c','calc.c'],'lvselect':['selector.c'],'lvgallery':['gallery.c'],
                 'lvdata':['data-viewer.c','data-plot.c','csv.c'],'lvedit':['editor.c'],
                 'fusedeshell':['shell.c','shell-catalog.c',str(ROOT/'libraries/ini/ini.c')]}
    for app in selected:
        extra=[]
        if app in ('lvedit','lvgallery'):
            extra=[one(path) for path in (ROOT/'libraries/text/text.c',PORT/'ui-editor.c',PORT/'ui-document.c')]
        command=[*cc,str(PORT/'gx.c'),*[str(PORT/name) for name in app_sources[app]],*common,'--float-format','-D','LV_CONF_INCLUDE_SIMPLE=1','-D','__func__="C16"','-o',str(apps/(app+'.fxe'))]
        for path in (source,config,PORT,ROOT/'libraries/ini'):command+=['-I',str(path)]
        for path in objects+extra:command+=['--object-ir',str(path)]
        subprocess.run(command,check=True,env=env)
    if a.desktop:
        subprocess.run([*cc,str(ROOT/'ports/fuzix/ripple/gterm.c'),*common,'-o',str(apps/'gterm.fxe')],check=True,env=env)
        selected.append('gterm')
    report={'status':'compiled','lvgl_source':str(base),'reused_units':len(reused),'compiled_units':compiled,'apps':{}}
    for name in [*selected,'screenshot',*(['desktop-probe'] if a.desktop else [])]:
        path=apps/(name+'.fxe');data=path.read_bytes()
        if data[:12]!=b'FUSEX001RXE1':raise RuntimeError('Not a freestanding RXE1 executable')
        report['apps'][name]={'path':str(path),'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
    (build/'build-result.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
