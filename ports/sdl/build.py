#!/usr/bin/env python3
"""Build the optional SDL software foundation, independently of either kernel."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
try:
    from .adapt import prepare, COMMON, ROOT, PORT
except ImportError:  # direct script invocation
    from adapt import prepare, COMMON, ROOT, PORT


def mixer_profile():
    """Record this repository's optional effects API and exact source inputs."""
    profile = json.loads((PORT/'mixer/PROVENANCE.json').read_text())
    profile['source_sha256'] = {
        name: hashlib.sha256((PORT/'mixer'/name).read_bytes()).hexdigest()
        for name in profile['files']
    }
    return profile


def library(target, build, compiler, fetch=False, jobs=4, mixer=False):
    """Prepare reusable software SDL sources and, for C16, cached object IR."""
    build.mkdir(parents=True, exist_ok=True)
    c16 = target == 'fuse-x'
    source = prepare(build, fetch, c16)
    units = [source / name for name in COMMON if not name.endswith('SDL_string.c')]
    units += [PORT / 'platform/support.c', PORT / 'platform/audio.c',
              PORT / 'c16/audio_formats.c',
              PORT / ('platform/pcm_audio.c' if c16 else 'platform/host_audio.c'),
              PORT / ('platform/gx.c' if c16 else 'platform/host.c')]
    if mixer:
        units.append(PORT / ('mixer/client.c' if mixer == 'process' else 'mixer/mixer.c'))
    includes = [source/'include',source/'src',source/'src/video',source/'src/events',
                source/'src/timer',PORT/'platform',PORT/'c16',PORT/'mixer']
    if c16:
        units.append(PORT/'c16/pixels.c')
    else:
        units += [source/'src/video'/n for n in ['SDL_blit_0.c','SDL_blit_1.c','SDL_blit_N.c',
                                                  'SDL_blit_A.c','SDL_RLEaccel.c']]
        units += [source/'src/video/dummy'/n for n in ['SDL_nullvideo.c','SDL_nullevents.c','SDL_nullmouse.c']]
    if not c16:
        return dict(source=source, includes=includes, units=units, objects=[])
    pinned=build/'rcc-pinned'
    if not pinned.exists() or pinned.read_bytes()!=compiler.read_bytes():shutil.copy2(compiler,pinned)
    sdk = [ROOT/'ports/fuzix/ripple/include',ROOT/'ports/fuzix/upstream/Library/include',
           ROOT/'runtime/include',ROOT/'ports/fuzix/ripple',ROOT/'ports/graphics/include']
    flags=['--target','native','-DRIPPLE_MMU=1','-DFUSE_X_USER=1']
    flags += [v for d in [*includes,*sdk] for v in ('-I',str(d))]
    digest=hashlib.sha256(pinned.read_bytes()+str(flags).encode())
    for directory in [*includes,*sdk]:
        for p in sorted(directory.rglob('*')):
            if p.suffix in ('.h','.inc'):digest.update(str(p).encode());digest.update(p.read_bytes())
    prefix=digest.digest();cache=build/'objects';cache.mkdir(exist_ok=True)
    def compile_one(path):
        key=hashlib.sha256(prefix+path.read_bytes()).hexdigest()[:24]
        ir=cache/(path.stem+'-'+key+'.ir')
        if not ir.exists():
            r=subprocess.run([str(pinned),'compile',str(path),*flags,'--emit-ir'],capture_output=True,
                             env=dict(os.environ,RUST_LOG='error'))
            (cache/(path.stem+'.log')).write_bytes(r.stderr)
            if r.returncode:raise RuntimeError(str(path)+'\n'+r.stderr.decode(errors='replace')[-3000:])
            ir.write_bytes(r.stdout)
        return ir
    print('Compiling SDL software foundation:',len(units),'units',flush=True)
    with ThreadPoolExecutor(max_workers=jobs) as pool:objects=list(pool.map(compile_one,units))
    return dict(source=source, includes=includes, units=units, objects=objects, compiler=pinned)


def build(target, build, compiler, fetch=False, app='surface', jobs=4):
    lib = library(target, build, compiler, fetch, jobs,
                  mixer='process' if app == 'mixer-ipc' else app in ('mixer','mixer-helper'))
    source, includes, units = lib['source'], lib['includes'], lib['units']
    c16 = target == 'fuse-x'
    if app == 'objects':
        return source
    test = PORT/'tests'/({'demo':'demo.c','bulk':'pixel-bulk.c',
                         'audio':'audio.c','audio-demo':'audio-demo.c',
                         'mixer':'mixer.c'}.get(app,'acceptance.c'))
    if app == 'mixer-helper': test = PORT/'mixer/helper.c'
    if app == 'mixer-ipc': test = PORT/'tests/mixer-ipc.c'
    output = build / ({'demo':'sdl-demo','bulk':'sdl-bulk','audio':'sdl-audio',
                       'audio-demo':'sdl-audio-demo','mixer':'sdl-mixer',
                       'mixer-helper':'sdl-mixerd','mixer-ipc':'sdl-mixer-ipc'}.get(app,'sdlcheck') + ('.fxe' if c16 else '-host'))
    if not c16:
        cmd = [*shlex.split(os.environ.get('CC','cc')),'-std=c99','-O2','-fno-strict-aliasing',
               *[v for d in includes for v in ('-I',str(d))], *map(str,units),str(test),'-o',str(output)]
        if sys.platform=='darwin':
            sdk=os.environ.get('SDKROOT')
            compatible=Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk')
            if not sdk and compatible.exists():sdk=str(compatible)
            if sdk:cmd+=['-isysroot',sdk]
        with (build/'host-build.log').open('w') as log:
            r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
        if r.returncode:raise RuntimeError((build/'host-build.log').read_text()[-5000:])
        return output
    pinned, objects = lib['compiler'], lib['objects']
    cmd=[sys.executable,str(ROOT/'fuse-x/cc.py'),str(test),'--compiler',str(pinned),
         '--static','--float-format','--build',str(build/'sdk'),'-o',str(output)]
    for d in includes:cmd+=['-I',str(d)]
    for ir in objects:cmd+=['--object-ir',str(ir)]
    with (build/'guest-link.log').open('w') as log:
        r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
    if r.returncode:raise RuntimeError((build/'guest-link.log').read_text()[-6000:])
    return output


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target',choices=['host','fuse-x'],default='fuse-x')
    p.add_argument('--app',choices=['surface','demo','bulk','objects','audio','audio-demo','mixer','mixer-helper','mixer-ipc'],default='surface')
    p.add_argument('--build',type=Path,default=ROOT/'build/sdl')
    p.add_argument('--compiler',type=Path,default=ROOT/'target/release/rcc')
    p.add_argument('--fetch',action='store_true')
    p.add_argument('--stage',type=Path,help='explicitly stage the demo and notices in a host share')
    p.add_argument('-j','--jobs',type=int,default=4)
    args=p.parse_args();args.build=args.build.resolve();args.build.mkdir(parents=True,exist_ok=True)
    if args.stage and (args.target!='fuse-x' or args.app not in ('demo','audio-demo')):
        p.error('--stage is only available for a Fuse-X demo')
    # Source generation and SDK reuse must not race another invocation in the
    # same build folder. Per-translation-unit compilation remains parallel.
    with (args.build/'.build.lock').open('w') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        output=build(args.target,args.build,args.compiler.resolve(),args.fetch,args.app,args.jobs)
        if args.app!='objects':
            info=dict(upstream=json.loads((PORT/'PROVENANCE.json').read_text()),
                      target=args.target,app=args.app,linkage='static',
                      executable_sha256=hashlib.sha256(output.read_bytes()).hexdigest(),
                      executable_octets=output.stat().st_size,
                      abi='C16 logical octets' if args.target=='fuse-x' else 'host byte ABI')
            if args.app in ('mixer','mixer-helper','mixer-ipc'):
                info['mixer'] = mixer_profile()
            info_path=args.build/(output.name+'.json')
            info_path.write_text(json.dumps(info,indent=2)+'\n')
            if args.stage:
                stage=args.stage.resolve();stage.mkdir(parents=True,exist_ok=True)
                temporary=stage/('.'+output.name+'.new')
                shutil.copy2(output,temporary);temporary.replace(stage/output.name)
                notices=stage/'sdl';notices.mkdir(exist_ok=True)
                for path in [PORT/'README.md',PORT/'PROVENANCE.json',info_path,
                             args.build/'adaptation-c16.json',*sorted((args.build/'licenses').glob('*'))]:
                    shutil.copy2(path,notices/path.name)
        print(output)


if __name__=='__main__':main()
