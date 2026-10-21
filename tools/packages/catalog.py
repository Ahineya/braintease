#!/usr/bin/env python3
"""Package built apps without rebuilding a kernel or touching any OS disk.

Python orchestrates host builds/staging; ports/pkg owns package serialization.
Native payloads are unchanged: only FXE module dependency metadata is rebound
into a versioned, package-owned library namespace.
"""
from pathlib import Path
import argparse
import base64
import collections
import hashlib
import json
import re
import struct
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
TARGETS={'fuse-x':'fuse-x-arm64-macos-c16','x64':'riplix-x64-c16'}
ALIASES={'vi':'rift','mc':'rfl-fm','mv':'cp','ln':'cp','tcl':'picol','tclsh':'picol','sokoban':'sok','nslookup':'host','[':'test'}
PROTECTED={'sh','svcd','console-session'}
DEPRECATED={'puzzles':'retired combined puzzle launcher; individual games are packaged'}
DATA={'rfl-fm':['commander-data'],'rift':['rift-data'],'sok':['sokoban-data'],
      'basic':['basic-data'],'lynx':['lynx-data','ca-certificates'],
      'chat':['ca-certificates'],'htget':['ca-certificates'],'netsurf':['netsurf-data','ca-certificates'],
      'kolf':['kolf-data'],'kolfpack':['kolf-data'],'klotski':['klotski-data'],'shikaku':['shikaku-data'],
      'nxlaunch':['nxlaunch-data'],
      'fdeshowcase':['fusede-data'],'nanowm':['fusede-data'],'make':['make-data'],'man':['man-pages']}
GUI={'gterm','nano-X','nanowm','nxlaunch','nxterm','nxclock','nxeyes','nxmsg','nxselect','nxcalc','nxtetris','nxview','kolf','klotski','shikaku','startx','nxdraw','fdeshowcase','fdectl','kolfpack','netsurf'}
PUZZLES={g['id'] for g in json.loads((ROOT/'ports/puzzles/catalog.json').read_text())}
GUI |= PUZZLES
LARGE={'chat','netsurf','lynx','rift','telegram','music','wavplay','basic','rfl-fm','mux','picol'}

def digest(data):return hashlib.sha256(data).hexdigest()
def canonical(value):return json.dumps(value,ensure_ascii=False,separators=(',',':')).encode()
def decode(data):
    if data[:8]!=b'FUSEX001':raise ValueError('not an FXE executable')
    length,code=struct.unpack_from('<II',data,8)
    if length>len(data)-16 or code>len(data)-16-length:raise ValueError('invalid FXE lengths')
    return json.loads(data[16:16+length]),data[16+length:],code

def interface(module):
    symbols=sorted((s['name'],s['function'],s['signature']) for s in module['exports'])
    return digest(canonical(['c16-native-continuations-3',symbols]))

def encode(metadata,tail,code):
    data=canonical(metadata)
    return b'FUSEX001'+struct.pack('<II',len(data),code)+data+tail

def valid_path(path):
    return len(path)<256 and not path.startswith('/') and all(p not in ('','.','..') and len(p)<=30 for p in path.split('/'))

class Catalog:
    def __init__(self,build,out,engine,target):
        self.build,self.out,self.engine,self.target=build,out,engine,TARGETS[target]
        self.kind=target;self.packages={};self.libraries={};self.excluded={};self.apps=[]
        self.candidates=collections.defaultdict(list)
        if target=='fuse-x':
            for folder in [build/'lib',*sorted(build.glob('**/lib')),ROOT/'build/pkg/fuse-sdk/lib']:
                for file in sorted(folder.glob('*.fxl.*')):
                    try:
                        data=file.read_bytes();m,tail,code=decode(data)
                        if m['arch']=='aarch64' and m['os']=='macos':
                            self.candidates[(m['module']['name'],interface(m['module']))].append((file,data))
                    except (ValueError,KeyError,json.JSONDecodeError):continue

    def package(self,name,files,depends=(),target=None):
        if len(name)>24 or not re.fullmatch(r'[a-zA-Z0-9._+-]+',name):raise ValueError(f'package name: {name}')
        deps=sorted(set(depends));dependency=','.join(deps)
        if len(dependency)>255 or len(deps)>16:raise ValueError(f'dependency bound: {name}')
        files=sorted(files,key=lambda f:f['name'])
        if len(files)>128 or len({f['name'] for f in files})!=len(files):raise ValueError(f'file count/duplicates: {name}')
        identity=hashlib.sha256(canonical([target or self.target,dependency]))
        for f in files:
            if not valid_path(f['name']):raise ValueError(f'path: {f["name"]}')
            identity.update(canonical([f['name'],f['mode'],digest(f['data'])]))
        version='0.'+identity.hexdigest()[:20]
        key=(name,target or self.target)
        if key in self.packages:
            if self.packages[key]['version']!=version:raise ValueError(f'duplicate ownership: {name}')
            return self.packages[key]
        filename=digest((name+' '+key[1]).encode())[:24]+'.rpk';output=self.out/filename
        stamp=output.with_suffix('.json')
        expected=dict(name=name,version=version,target=key[1],depends=deps,files=[f['name'] for f in files])
        cached=False
        if output.exists() and stamp.exists():
            prev=json.loads(stamp.read_text())
            cached=all(prev.get(k)==v for k,v in expected.items()) and prev.get('sha256')==digest(output.read_bytes())
        if not cached:
            with tempfile.TemporaryDirectory(prefix='pkg-stage-',dir=self.out) as tmp:
                work=Path(tmp);stage=work/'root';stage.mkdir()
                for f in files:
                    dest=stage/f['name'];dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(f['data']);dest.chmod(f['mode'])
                meta=work/'manifest';meta.write_text(f'name={name}\nversion={version}\ntarget={key[1]}\ndepends={dependency}\n')
                subprocess.run([str(self.engine),'create',str(meta),str(stage),str(output)],check=True)
            expected['sha256']=digest(output.read_bytes());stamp.write_text(json.dumps(expected,indent=2)+'\n')
        else:expected['sha256']=prev['sha256']
        expected['archive']=str(output);self.packages[key]=expected
        print(f'Package: {name} [{key[1]}] '+('cached' if cached else f'{output.stat().st_size:,} bytes'),flush=True)
        return expected

    def group(self,name,files,depends=(),target=None):
        # Source/header trees exceed the archive's per-package bound; stable
        # bounded parts let a changed header update its own resource package.
        if len(files)<=100:return self.package(name,files,depends,target)
        parts=[]
        for i in range(0,len(files),100):
            part=self.package(f'{name}-{i//100+1}',files[i:i+100],target=target)
            parts.append(part['name']+'@*')
        return self.collection(name,parts,depends,target)

    def collection(self,name,deps,extra=(),target=None):
        deps=sorted(set([*deps,*extra]))
        if len(','.join(deps))<=255 and len(deps)<=16:return self.package(name,[],deps,target)
        parts=[]
        for i in range(0,len(deps),5):
            part=self.collection('bundle-'+digest(canonical(deps[i:i+5]))[:16],deps[i:i+5],target=target);parts.append(part['name']+'@*')
        return self.collection(name,parts,target=target)

    def library(self,name,expected):
        key=(name,expected)
        if key in self.libraries:return self.libraries[key]
        matches=self.candidates.get(key,[])
        if not matches:raise ValueError(f'missing matching shared library {name} interface={expected}')
        source,data=matches[0];m,tail,code=decode(data)
        deps=self.rebind(m)
        closure=digest(encode(m,tail,code))
        unique=name+'.'+closure[:16]
        m['module']['name']=unique
        payload=encode(m,tail,code)
        path='usr/lib/fxp/'+closure[:24]+'/'+source.name
        pkg=self.package('fxlib-'+closure[:16],[dict(name=path,mode=0o644,data=payload)],deps+['unicode-data@*','system-licenses@*'])
        result=(unique,'/'+path,pkg['name']+'@'+pkg['version'])
        self.libraries[key]=result
        return result

    def rebind(self,m):
        module=m.get('module');deps=[];mapping={}
        if not module:return deps
        for dep in module['dependencies']:
            old=dep['name'];unique,path,package=self.library(old,dep['interface'])
            dep['name'],dep['path']=unique,path;mapping[old]=unique;deps.append(package)
        for symbol in module['imports']:symbol['library']=mapping[symbol['library']]
        return deps

    def application(self,name,source,extra=(),aliases=()):
        original=source.read_bytes()
        if self.kind=='x64':
            if original[:8]!=b'FUSEX001' or struct.unpack_from('<I',original,8)[0]!=0x31455852:raise ValueError(f'not an x64 RXE1: {source}')
            payload=original;deps=[]
        else:
            m,tail,code=decode(original)
            if (m['arch'],m['os'])!=('aarch64','macos'):raise ValueError(f'target mismatch: {source}')
            deps=self.rebind(m);payload=encode(m,tail,code) if m.get('module') else original
        if name=='netsurf':deps.append('htget@*')
        if self.kind=='fuse-x' and name in GUI and name not in {'nano-X','kolfpack','gterm'}:deps.append('nano-X@*')
        if name=='startx':deps.extend(['gterm@*','nanowm@*','nxlaunch@*'])
        deps += [f'{d}@*' for d in DATA.get(name,[])]+list(extra)+['system-licenses@*']
        files=[]
        for command in [name,*aliases]:
            files.append(dict(name='usr/bin/'+command,mode=0o755,data=payload))
            if command not in PROTECTED:
                files.append(dict(name='bin/'+command,mode=0o755,data=f'#!/bin/sh\nexec /usr/bin/{command} "$@"\n'.encode()))
        pkg=self.package(name,files,deps)
        self.apps.append(name)
        return pkg

    def run(self):
        raw=json.loads(subprocess.check_output(['node',str(ROOT/'tools/packages/resources.mjs'),str(self.build)]))
        compiler=raw.pop('compiler')
        for name,files in raw.items():
            entries=[dict(name=f['name'].lstrip('/'),mode=f['mode'],data=base64.b64decode(f['data'])) for f in files]
            self.group(name,sorted(entries,key=lambda f:f['name']),target='any')
        candidates=sorted((self.build/'apps').glob('*.fxe'))
        for p in candidates:
            name=p.stem
            if name in ALIASES:continue
            if name in DEPRECATED:self.excluded[name]=DEPRECATED[name];continue
            if name!='test' and re.search(r'check|(^|-)test($|-)',name):self.excluded[name]='diagnostic/test executable';continue
            self.application(name,p,aliases=[a for a,t in ALIASES.items() if t==name])
        if compiler and self.kind=='fuse-x':
            binaries=[];resources=[]
            for e in compiler:
                file=dict(name=e['name'].lstrip('/'),mode=e['mode']&0o777,data=base64.b64decode(e['data']))
                if file['name'].startswith('bin/'):binaries.append(file)
                else:resources.append(file)
            self.group('rcc-data',sorted(resources,key=lambda f:f['name']))
            names=[]
            for f in binaries:
                name=f['name'].split('/')[-1]
                with tempfile.TemporaryDirectory(prefix='pkg-compiler-',dir=self.out) as tmp:
                    p=Path(tmp)/name;p.write_bytes(f['data']);self.application(name,p,extra=['rcc-data@*']+([n['name'].split('/')[-1]+'@*' for n in binaries if n['name']!=f['name']] if name=='cc' else []))
                names.append(name+'@*')
            self.collection('c-toolchain',names+['make@*'])
        gui=GUI if self.kind=='fuse-x' else set()
        for category,selected in [('gui-apps',[n for n in self.apps if n in gui]),('unix-apps',[n for n in self.apps if n not in gui|LARGE and not n.startswith('rcc-') and n!='cc']),('developer-apps',[n for n in self.apps if n in LARGE or n=='cc' or n.startswith('rcc-')])]:
            if selected:self.collection(category,[n+'@*' for n in selected])
        # Package the manager with the same isolated library closure as apps.
        if self.kind=='fuse-x' and (ROOT/'build/pkg/pkg.fxe').exists():
            m,tail,code=decode((ROOT/'build/pkg/pkg.fxe').read_bytes());deps=self.rebind(m)
            self.package('pkg',[dict(name='usr/bin/pkg',mode=0o755,data=encode(m,tail,code)),dict(name='usr/libexec/pkg-engine',mode=0o755,data=(ROOT/'build/pkg/pkg-engine.fxe').read_bytes())],deps+['system-licenses@*'])
        elif self.kind=='x64' and (ROOT/'build/pkg/pkg-engine-x64.rxe').exists():
            self.package('pkg',[dict(name='usr/bin/pkg',mode=0o755,data=(ROOT/'build/pkg/pkg-engine-x64.rxe').read_bytes())])
        self.collection('apps',[n+'@*' for n in self.apps])
        report=dict(target=self.target,applications=sorted(self.apps),aliases={a:t for a,t in ALIASES.items() if t in self.apps},excluded=self.excluded,packages=list(self.packages.values()))
        (self.out/'catalog.json').write_text(json.dumps(report,indent=2)+'\n')
        # Remove only stale archives generated by this catalog, never arbitrary
        # build outputs or developer disks. Payload retention is on the mini.
        keep={Path(p['archive']).name for p in self.packages.values()}
        for archive in self.out.glob('*.rpk'):
            if archive.name not in keep:archive.unlink();archive.with_suffix('.json').unlink(missing_ok=True)
        print(f'Catalog complete: {len(self.apps)} apps, {len(self.packages)} packages, {len(self.excluded)} diagnostics/retired programs excluded',flush=True)
        return report

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--target',choices=TARGETS,default='fuse-x')
    ap.add_argument('--build',type=Path)
    ap.add_argument('--output',type=Path)
    ap.add_argument('--engine',type=Path,default=ROOT/'build/pkg/host/pkg-engine')
    args=ap.parse_args()
    build=(args.build or ROOT/('fuse-x/build' if args.target=='fuse-x' else 'build/x86_64-network/userspace-bounded')).resolve()
    output=(args.output or ROOT/'build/pkg/catalog'/args.target).resolve();output.mkdir(parents=True,exist_ok=True)
    if not args.engine.exists():subprocess.run(['sh',str(ROOT/'ports/pkg/build.sh')],check=True)
    Catalog(build,output,args.engine.resolve(),args.target).run()

if __name__=='__main__':
    try:main()
    except (OSError,ValueError,KeyError,subprocess.CalledProcessError) as e:raise SystemExit('Package catalog: '+str(e))
