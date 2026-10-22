#!/usr/bin/env python3
"""Build the original LGeneral engine with SDL software graphics for Fuse-X."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
from prepare import prepare,PORT,ROOT,UNITS,UTIL
sys.path.insert(0,str(ROOT))
from ports.sdl.build import library, mixer_profile, build as build_sdl


def build(args):
    folder=args.build.resolve();folder.mkdir(parents=True,exist_ok=True)
    c16=args.target=='fuse-x'
    source,origins=prepare(folder,args.fetch,c16)
    from assets import prepare_assets
    report=prepare_assets(origins['engine'],origins['data'],folder/'data')
    lib=library(args.target,folder/'sdl',args.compiler.resolve(),args.fetch,args.jobs,
                mixer='process' if c16 else True)
    helper=None
    if c16:
        helper=build_sdl(args.target,folder/'mixer-helper',lib['compiler'],args.fetch,'mixer-helper',args.jobs)
        shutil.copy2(helper,folder/'sdl-mixerd.fxe');helper=folder/'sdl-mixerd.fxe'
    units=[source/'src'/(n+'.c') for n in UNITS]+[source/'util'/(n+'.c') for n in UTIL]
    includes=[source,source/'src',source/'util',*lib['includes']]
    flags=['-DHAVE_CONFIG_H=1','-DNDEBUG=1','-DWITH_SOUND=1']
    if args.engine_test:flags+=['-DLGENERAL_ENGINE_TEST=1']
    output=folder/('lgeneral-test' if args.engine_test else 'lgeneral')
    if not c16:
        cmd=[*shlex.split(os.environ.get('CC','cc')),'-std=c99','-O2','-fno-strict-aliasing',
             *flags,*[v for d in includes for v in ('-I',str(d))],*map(str,[*units,*lib['units']]),'-lm','-o',str(output)]
        if sys.platform=='darwin':
            compatible=Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk')
            sdk=os.environ.get('SDKROOT') or (str(compatible) if compatible.exists() else None)
            if sdk:cmd+=['-isysroot',sdk]
        log=folder/'host-build.log'
        with log.open('w') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT)
        if r.returncode:raise RuntimeError(log.read_text()[-6000:])
    else:
        output=output.with_suffix('.fxe');compiler=lib['compiler']
        sdk=[ROOT/'ports/fuzix/ripple/include',ROOT/'ports/fuzix/upstream/Library/include',ROOT/'runtime/include',ROOT/'ports/fuzix/ripple',ROOT/'ports/graphics/include']
        flags=['--target','native','-DRIPPLE_MMU=1','-DFUSE_X_USER=1',*flags]
        flags += [v for d in [*includes,*sdk] for v in ('-I',str(d))]
        digest=hashlib.sha256(compiler.read_bytes()+str(flags).encode())
        for d in [*includes,*sdk]:
            for p in sorted(d.rglob('*')):
                if p.suffix in ('.h','.inc'):digest.update(str(p).encode());digest.update(p.read_bytes())
        prefix=digest.digest();cache=folder/'objects';cache.mkdir(exist_ok=True)
        def compile_one(path):
            key=hashlib.sha256(prefix+path.read_bytes()).hexdigest()[:24];ir=cache/(path.stem+'-'+key+'.ir')
            if not ir.exists():
                r=subprocess.run([str(compiler),'compile',str(path),*flags,'--emit-ir'],capture_output=True,env=dict(os.environ,RUST_LOG='error'))
                (cache/(path.stem+'.log')).write_bytes(r.stderr)
                if r.returncode:raise RuntimeError(str(path)+'\n'+r.stderr.decode(errors='replace')[-3000:])
                ir.write_bytes(r.stdout)
            return ir
        print('Compiling LGeneral:',len(units),'units',flush=True)
        failures=[];objects=[]
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            futures=[pool.submit(compile_one,p) for p in units if p.name != 'main.c']
            for future in futures:
                try:objects.append(future.result())
                except RuntimeError as e:failures.append(str(e))
        if failures:raise RuntimeError('\n'.join(failures))
        # The SDK wraps the real C main; other game/SDL units are cached IR.
        cmd=[sys.executable,str(ROOT/'fuse-x/cc.py'),str(source/'src/main.c'),'--compiler',str(compiler),'--static','--float-format','--build',str(folder/'sdk'),'-o',str(output),'-DHAVE_CONFIG_H=1','-DNDEBUG=1','-DWITH_SOUND=1']
        if args.engine_test:cmd+=['-DLGENERAL_ENGINE_TEST=1']
        for d in includes:cmd+=['-I',str(d)]
        for ir in [*objects,*lib['objects']]:cmd+=['--object-ir',str(ir)]
        log=folder/'guest-link.log'
        with log.open('w') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT)
        if r.returncode:raise RuntimeError(log.read_text()[-8000:])
    info=dict(upstream=json.loads((PORT/'PROVENANCE.json').read_text()),target=args.target,
              sdl=json.loads((ROOT/'ports/sdl/PROVENANCE.json').read_text()),
              mixer=mixer_profile(),
              mixer_backend='process' if c16 else 'local',
              mixer_helper_sha256=hashlib.sha256(helper.read_bytes()).hexdigest() if helper else None,
              compiler_sha256=hashlib.sha256(lib['compiler'].read_bytes()).hexdigest() if c16 else None,
              abi='C16 logical octets; 32-bit game integers' if c16 else 'host octets; 32-bit game integers',
              executable_sha256=hashlib.sha256(output.read_bytes()).hexdigest(),executable_octets=output.stat().st_size,
              data_manifest_sha256=hashlib.sha256((folder/'data/assets-manifest.json').read_bytes()).hexdigest())
    (folder/(output.name+'.json')).write_text(json.dumps(info,indent=2)+'\n')
    if args.stage:
        stage=args.stage.resolve();stage.mkdir(parents=True,exist_ok=True)
        temporary=stage/'.lgeneral.fxe.new'
        shutil.copy2(output,temporary);temporary.replace(stage/'lgeneral.fxe')
        temporary=stage/'.sdl-mixerd.fxe.new'
        shutil.copy2(helper,temporary);temporary.replace(stage/'sdl-mixerd.fxe')
        shutil.copytree(folder/'data',stage/'lgeneral',dirs_exist_ok=True)
        for name in ('README.md','PROVENANCE.json'):shutil.copy2(PORT/name,stage/'lgeneral'/name)
        shutil.copy2(folder/(output.name+'.json'),stage/'lgeneral/build.json')
        shutil.copy2(folder/'adaptation-c16.json',stage/'lgeneral/adaptation-c16.json')
        shutil.copy2(folder/'sdl/licenses/SDL-COPYING.txt',stage/'lgeneral/licenses/SDL-COPYING.txt')
        mixer_notices=stage/'lgeneral/mixer';mixer_notices.mkdir(exist_ok=True)
        for name in ('README.md','PROVENANCE.json'):
            shutil.copy2(ROOT/'ports/sdl/mixer'/name,mixer_notices/name)
    return output


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target',choices=['fuse-x','host'],default='fuse-x')
    p.add_argument('--build',type=Path,default=ROOT/'build/lgeneral')
    p.add_argument('--compiler',type=Path,default=ROOT/'target/release/rcc')
    p.add_argument('--fetch',action='store_true');p.add_argument('--stage',type=Path)
    p.add_argument('--engine-test',action='store_true');p.add_argument('-j','--jobs',type=int,default=4)
    args=p.parse_args();args.build=args.build.resolve();args.build.mkdir(parents=True,exist_ok=True)
    if args.stage and (args.target!='fuse-x' or args.engine_test):p.error('Stage the interactive Fuse-X build only')
    with (args.build/'.build.lock').open('w') as f:
        fcntl.flock(f,fcntl.LOCK_EX);print(build(args))


if __name__=='__main__':main()
