#!/usr/bin/env python3
"""Link guest applications against versioned shared libraries, or legacy RXE1.

Accepts rlink's usual RXE arguments. Kernel/BIOS links still use rlink directly.
The shared SDK has its own object directory so parallel application builds do
not race Make recipes that produce the ordinary static SDK.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from shared import sdk
from shared.link import load_objects,pack

ROOT=Path(__file__).resolve().parents[3]


def mode_stamp(build):
    path=build.resolve()/'.shared-mode';path.parent.mkdir(parents=True,exist_ok=True)
    value='static\n' if os.environ.get('RIPLIX_SHARED_LIBRARIES','1')=='0' else 'shared\n'
    # A packer change must relink existing applications as well as the SDK.
    sources=[Path(__file__),*sorted(Path(__file__).with_name('shared').glob('*.py'))]
    value+=hashlib.sha256(b''.join(p.read_bytes() for p in sources)).hexdigest()+'\n'
    if not path.exists() or path.read_text()!=value:path.write_text(value)
    return path


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,required=True)
    p.add_argument('--linker',type=Path,default=ROOT/'src/ripple-asm/target/release/rlink')
    p.add_argument('--compiler',type=Path,default=ROOT/'target/release/rcc')
    p.add_argument('--stamp-only',action='store_true')
    p.add_argument('arguments',nargs=argparse.REMAINDER)
    a=p.parse_args();build=a.build.resolve();linker=a.linker.resolve();assembler=linker.with_name('rasm')
    if a.stamp_only:print(mode_stamp(build));return
    arguments=a.arguments[1:] if a.arguments[:1]==['--'] else a.arguments
    if os.environ.get('RIPLIX_SHARED_LIBRARIES','1')=='0':
        subprocess.run([str(linker),*arguments],check=True);return
    q=argparse.ArgumentParser();q.add_argument('inputs',type=Path,nargs='+');q.add_argument('-o',type=Path,required=True)
    q.add_argument('-f',default='rxe');q.add_argument('--bank-size',default='64000');q.add_argument('--map',type=Path)
    options=q.parse_args(arguments)
    if options.f!='rxe' or options.bank_size!='64000':raise ValueError('shared linking requires RXE and 64000-cell banks')
    directory=build/'lib';directory.mkdir(parents=True,exist_ok=True)
    command=['make','-s','--no-print-directory','-C',str(ROOT/'ports/fuzix'),f'BUILD={build}','RFL_CURSES=1','RFL_UNICODE=1']
    info=json.loads(subprocess.check_output([*command,'rfl-sdk-info'],text=True))
    floats=json.loads(subprocess.check_output([*command,'float-sdk-info'],text=True))
    entry=Path(info['entry']).resolve();inputs=[p.resolve() for p in options.inputs]
    if entry not in inputs:raise ValueError('shared link is missing the guest startup object')
    core={Path(p).resolve() for p in info['libraries'] if '/curses/' not in p}
    core.update(Path(p).resolve() for p in floats['libraries'])
    core.add(build/'libc/format.pobj');core.add(build/'libc/unicode.pobj')
    companions={Path(p).resolve() for p in info['libraries'] if '/curses/' in p}
    want_curses=bool(set(inputs)&companions)
    libraries,_=sdk.prepare(build/'shared-sdk',a.compiler.resolve(),assembler,linker,want_curses)
    # Publish only complete packages. Each package names guest paths, so moving
    # it from the private build cache to the install directory changes no ABI.
    def publish(path):
        target=directory/path.name
        with target.with_suffix(target.suffix+'.lock').open('a') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX)
            if not target.exists() or target.read_bytes()!=path.read_bytes():
                temporary=target.with_name('.'+target.name+'.new');shutil.copyfile(path,temporary);temporary.replace(target)
        return target
    libraries=[(publish(path),guest) for path,guest in libraries]
    application=[p for p in inputs if p!=entry and p not in core and p not in companions]
    # TLS manifests list library IR independently of each consumer's own IR.
    # Classify those exact objects rather than guessing from symbol prefixes.
    manifests={p.parent/'objects.json' for p in application}
    tls=set()
    for manifest in manifests:
        if manifest.exists() and manifest.parent.name=='ripple':
            records=json.loads(manifest.read_text())
            if any(Path(p).stem=='ripple_tls' for p in records):
                tls.update(Path(p).with_suffix('.pobj').resolve() for p in records)
    objects=[p for p in application if p in tls]
    if objects:
        output=directory/'libtls.rsl.1';stamp=output.with_suffix('.1.sha256')
        with output.with_suffix('.1.lock').open('a') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX)
            key=hashlib.sha256()
            for path in [*objects,libraries[0][0],*sorted((ROOT/'ports/fuzix/tools/shared').glob('*.py'))]:key.update(path.read_bytes())
            fingerprint=key.hexdigest()
            if not output.exists() or not stamp.exists() or stamp.read_text()!=fingerprint:
                public={n for obj in load_objects(objects) for n in (obj.get('rcc') or {}).get('exports',[])}
                exports=set()
                for header in (ROOT/'ports/tls').glob('ripple_*.h'):
                    exports.update(re.findall(r'\b(ripple_(?:tls|crypto|http)_\w+)\s*\(',header.read_text()))
                pack(objects,output,linker=linker,assembler=assembler,name=output.name,
                     exports=sorted(public&exports),dependencies=[libraries[0]])
                stamp.write_text(fingerprint)
        libraries.append((output,'/lib/riplix/'+output.name));application=[p for p in application if p not in tls]
    pack(application,options.o,linker=linker,assembler=assembler,dependencies=libraries,startup=entry,
         map_output=options.map)


if __name__=='__main__':
    try:main()
    except (OSError,ValueError,subprocess.CalledProcessError) as error:
        sys.exit('Shared RXE link failed: '+str(error))
