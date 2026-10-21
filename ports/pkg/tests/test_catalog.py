#!/usr/bin/env python3
"""Validate the full built catalog, its dependency closure and seed ownership."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools/packages'))
from catalog import decode, interface

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--catalog',type=Path,default=ROOT/'build/pkg/catalog/fuse-x/catalog.json')
    args=ap.parse_args();report=json.loads(args.catalog.read_text())
    packages={p['name']:p for p in report['packages']}
    paths={};ordered=[];seen=set();visiting=set()
    def walk(name):
        if name in seen:return
        assert name not in visiting,('cycle',name)
        visiting.add(name);p=packages[name]
        for dep in p['depends']:
            n,v=dep.split('@');assert v=='*' or packages[n]['version']==v,(name,dep)
            walk(n)
        visiting.remove(name);seen.add(name);ordered.append(p)
    for name in packages:walk(name)
    for p in ordered:
        assert hashlib.sha256(Path(p['archive']).read_bytes()).hexdigest()==p['sha256']
        for path in p['files']:
            assert path not in paths,(path,paths.get(path),p['name'])
            paths[path]=p['name']
    engine=ROOT/'build/pkg/host/pkg-engine'
    with tempfile.TemporaryDirectory(prefix='pkg-catalog-') as tmp:
        work=Path(tmp).resolve();root=work/'root';root.mkdir()
        for p in ordered:
            subprocess.run([str(engine),'--root',str(root),'--target',report['target'],'install',p['archive']],check=True,stdout=subprocess.DEVNULL)
        if report['target']=='fuse-x-arm64-macos-c16':
            # All original native payload and initializer bytes remain intact.
            # SONAME/dependency rebinding must agree exactly across every edge.
            modules={}
            for path in paths:
                if path.startswith('usr/lib/fxp/'):
                    m,_,_=decode((root/path).read_bytes());modules[m['module']['name']]=(m,'/'+path)
            for path in paths:
                if not path.startswith('usr/bin/'):continue
                data=(root/path).read_bytes()
                if not data.startswith(b'FUSEX001'):continue
                m,tail,code=decode(data)
                for dep in (m.get('module') or {}).get('dependencies',[]):
                    library,installed=modules[dep['name']]
                    assert dep['path']==installed and interface(library['module'])==dep['interface']
                source=ROOT/'fuse-x/build/apps'/(path.split('/')[-1]+'.fxe')
                if source.exists():assert decode(source.read_bytes())[1:]==(tail,code),path
        # Test the host seed installer's ownership guard on a real C16 image.
        managed='usr/share/pkg-test/data';config='etc/pkg-test.conf';bridge='bin/pkg-test'
        db='RIPDB1\nname=fixture\nversion=1\ntarget=any\ndepends=\nfiles=3\nhash='+'a'*64+'\n'
        for path in (managed,config,bridge):db+='file=644,3,'+'b'*64+','+path+'\n'
        js=r'''
import {rootImage} from './ports/fuzix/tools/disk.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
import {checkRoot} from './ports/fuzix/tools/fsck.mjs';
const [db,paths]=process.argv.slice(1), names=JSON.parse(paths);
const initial=[{name:'/var/lib/pkg/db/fixture.db',data:Buffer.from(db)},...names.map(name=>({name:'/'+name,data:Buffer.from('old')}))];
const before=rootImage(initial,4096,256);
const after=installRoot(before,names.map(name=>({name:'/'+name,data:Buffer.from('new')})));
if(!before.equals(after))throw Error('host seed overwrote package-owned files');
if(checkRoot(after).issues.length)throw Error('invalid filesystem');
'''
        subprocess.run(['node','--input-type=module','-e',js,db,json.dumps([managed,config,bridge])],cwd=ROOT,check=True)
        subprocess.run([str(engine),'--root',str(root),'--discard-previous','clean'],check=True)
    print(f"Full catalog validated: {len(report['applications'])} applications, {len(packages)} packages; dependency closure, all installed paths, payload bytes and seed ownership")
if __name__=='__main__':main()
