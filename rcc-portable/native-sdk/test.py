#!/usr/bin/env python3
"""Rebuild the bootstrap libc from raw C inside private x86 Riplix disks.

Only compiler tools and headers/sources are seeded; no host-built libc objects
are included. Reboot and same-object independent host linking check outputs.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
PORTABLE=HERE.parent
sys.path.insert(0,str(PORTABLE/'qbe'))
from test_x86 import run_guest,file,EXTRACTOR


def verify_host(sdk):
    actual=sdk/'guest';linker=PORTABLE/'build/rcc-x64-ld'
    for name,objects in [('probe',['probe.o']),('variadic',['main.o','callee.o'])]:
        reference=actual/(name+'.host-link')
        subprocess.run([str(linker),'--sdk',str(actual),*[str(actual/n) for n in objects],'-o',str(reference)],check=True)
        assert reference.read_bytes()==(actual/name).read_bytes(),name+' host/guest link differs'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk',type=Path,default=ROOT/'build/portable-native-sdk')
    p.add_argument('--output',type=Path,default=ROOT/'build/portable-native-x64')
    p.add_argument('--timeout',type=int,default=3600)
    p.add_argument('--jobs',type=int,choices=range(1,5),default=3)
    mode=p.add_mutually_exclusive_group()
    mode.add_argument('--guest-only',action='store_true',help='defer independent host linking until outputs are copied to the host')
    mode.add_argument('--verify-host-only',action='store_true',help='finish deferred host parity for an existing guest result')
    a=p.parse_args();sdk=a.sdk.resolve();out=a.output.resolve()
    if a.verify_host_only:
        report=sdk/'guest-result.json';result=json.loads(report.read_text())
        if result['status'] not in ('guest-passed','passed'):
            raise RuntimeError('guest acceptance must pass before host verification')
        verify_host(sdk);result.update(status='passed',host_link_parity=True)
        report.write_text(json.dumps(result,indent=2)+'\n')
        print('Independent host/guest linking parity passed for the guest-built SDK/programs')
        return
    (sdk/'guest-result.json').unlink(missing_ok=True)
    rows=json.loads((sdk/'manifest.json').read_text())
    files=[]
    for folder in ['include','tree','generated']:
        files += [file('/tmp/'+str(src.relative_to(sdk)),src) for src in sorted((sdk/folder).rglob('*')) if src.is_file()]
    names=['rcc-c','rcc-qbe','rcc-native-cc','rcc-x64-ld','rcc-x64-ar']
    files += [file('/bin/'+n,PORTABLE/'build/apps'/(n+'-x86.fxe')) for n in names]
    files += [file('/bin/qbe',ROOT/'build/guest-native-assessment/qbe-x86.fxe'),file('/bin/rcc-x64-as',ROOT/'build/guest-native-assessment/rcc-x64-as-x86.fxe')]
    compile_files=files.copy()
    files += [file('/probe.c',HERE/'tests/probe.c')]
    files += [file('/argv.c',HERE/'tests/argv.c'),file('/cells.c',HERE/'tests/cells.c'),file('/cell-fallback.c',HERE/'cells.c')]
    files += [file('/qsort.c',ROOT/'ports/fuzix/upstream/Library/libs/qsort.c'),file('/sort-check.c',ROOT/'c-test/tests/runtime/test_native_sdk_qsort.c')]
    var=PORTABLE/'native/tests/variadic'
    files += [file('/'+n,var/n) for n in ['api.h','main.c','callee.c']]
    actual=sdk/'guest';actual.mkdir(exist_ok=True)
    fingerprint=hashlib.sha256()
    for f in compile_files:
        fingerprint.update(f['name'].encode());fingerprint.update(Path(f['path']).read_bytes())
    fingerprint.update((out/'normal/kernel.elf').read_bytes())
    fingerprint.update(json.dumps(rows,sort_keys=True).encode())
    key=fingerprint.hexdigest()
    groups=[[] for _ in range(a.jobs)];weights=[0]*a.jobs
    def cost(row):
        ssa=(sdk/row['object']).with_suffix('.ssa')
        return max(20000,ssa.stat().st_size if ssa.exists() else (sdk/row['source']).stat().st_size*24)
    for row in sorted(rows,key=cost,reverse=True):
        i=min(range(a.jobs),key=lambda n:weights[n]);groups[i].append(row);weights[i]+=cost(row)
    def unit(number,row):
        name=Path(row['object']).name
        dest=actual/'units'/Path(name).stem;dest.mkdir(parents=True,exist_ok=True)
        record=dest/'result.json';cached=json.loads(record.read_text()) if record.exists() else None
        if cached and cached['key']==key and cached['sources']==[row] and all(
            (dest/name).exists() and hashlib.sha256((dest/name).read_bytes()).hexdigest()==digest
            for name,digest in cached['objects'].items()) and set(cached['objects'])=={name}:
            print(f'SDK {name}: verified guest output reused',flush=True)
            return cached['commands']
        command=shlex.join(['rcc-native-cc','--sdk','/tmp',*row['flags'],'-c','/tmp/'+row['source'],'-o','/tmp/'+name])
        phase=f'sdk-source-{number}'
        disk,checks=run_guest(out,phase,compile_files,[(command+'; echo BUILD:$?','BUILD:0')],a.timeout)
        subprocess.run(['node','--input-type=module','-e',EXTRACTOR,str(disk),str(dest),json.dumps([name])],cwd=ROOT,check=True)
        (dest/'compile.log').write_bytes((out/(phase+'.log')).read_bytes())
        # Commit only closed, filesystem-checked outputs. A lost test process
        # can discard the current unit, never the preceding completed units.
        pending=record.with_suffix('.tmp')
        pending.write_text(json.dumps(dict(key=key,sources=[row],commands=checks,
            objects={name:hashlib.sha256((dest/name).read_bytes()).hexdigest()}),indent=2)+'\n')
        pending.replace(record)
        print(f'SDK {name}: guest output checkpointed',flush=True)
        return checks
    def batch(pair):
        number,group=pair
        return [check for row in group for check in unit(number,row)]
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        batches=list(pool.map(batch,enumerate(groups)))
    objects=[]
    for n,group in enumerate(groups):
        for row in group:
            name=Path(row['object']).name
            files.append(file('/tmp/'+name,actual/'units'/Path(name).stem/name))
            if row['object'].startswith('objects/'):objects.append('/tmp/'+name)
    # Preserve manifest order for deterministic archive/member selection.
    objects=['/tmp/'+Path(r['object']).name for r in rows if r['object'].startswith('objects/')]
    commands=[]
    fallback=Path(next(r['object'] for r in rows if r['source']=='generated/cells.c')).name
    sorting=Path(next(r['object'] for r in rows if r['source'].endswith('/qsort.c'))).name
    source_refreshes=[]
    # Rebuild the local fallback beside its direct ABI regression. This also
    # permits a focused fallback fix to reuse already checked library batches.
    for source,name,canonical,guest in [
        ('generated/cells.c',fallback,HERE/'cells.c','/cell-fallback.c'),
        (next(r['source'] for r in rows if r['source'].endswith('/qsort.c')),sorting,
         ROOT/'ports/fuzix/upstream/Library/libs/qsort.c','/qsort.c')]:
        if (sdk/source).read_bytes()!=canonical.read_bytes():
            commands.append((f'rcc-native-cc --sdk /tmp -c {guest} -o /tmp/{name}; echo REFRESH:$?','REFRESH:0'))
            source_refreshes.append(dict(source=str(canonical.relative_to(ROOT)),
                sha256=hashlib.sha256(canonical.read_bytes()).hexdigest(),object=name))
    # Feed the complete member list through a script, not the canonical TTY
    # input buffer. A normal Make recipe likewise invokes the shell directly.
    archive_script=actual/'archive-sdk.sh'
    archive_script.write_text(shlex.join(['rcc-x64-ar','-o','/tmp/libc.a',*objects])+'\n')
    files.append(file('/archive-sdk.sh',archive_script))
    commands += [('sh /archive-sdk.sh; echo ARCHIVE:$?','ARCHIVE:0'),
        ('rcc-native-cc --sdk /tmp -c /probe.c -o /tmp/probe.o; echo BUILD:$?','BUILD:0'),
        ('rcc-native-cc --sdk /tmp /tmp/probe.o -o /tmp/probe; echo LINK:$?','LINK:0'),
        ('/tmp/probe hello world; echo EXEC:$?','SDK_MAIN_OK hello world\nSDK_ATEXIT_OK\nEXEC:0'),
        ('rcc-native-cc --sdk /tmp -c /main.c -o /tmp/main.o; echo BUILD:$?','BUILD:0'),
        ('rcc-native-cc --sdk /tmp -c /callee.c -o /tmp/callee.o; echo BUILD:$?','BUILD:0'),
        ('rcc-native-cc --sdk /tmp /tmp/main.o /tmp/callee.o -o /tmp/variadic; echo LINK:$?','LINK:0'),
        ('/tmp/variadic; echo VARIADIC:$?','VARIADIC:0'),
        ('rcc-native-cc --sdk /tmp /argv.c -o /tmp/argv; echo BUILD:$?','BUILD:0'),
        ('/tmp/argv hello "x y"; echo ARGV:$?','SDK_ARGV_OK\nARGV:0'),
        ('rcc-native-cc --sdk /tmp /cells.c -o /tmp/cells; echo BUILD:$?','BUILD:0'),
        ('/tmp/cells; echo CELLS:$?','CELLS:0'),
        ('rcc-native-cc --sdk /tmp -DUSE_INSTALLED_QSORT /sort-check.c -o /tmp/sort-check; echo BUILD:$?','BUILD:0'),
        ('/tmp/sort-check; echo SORT:$?','YYY\nSORT:0')]
    disk,build=run_guest(out,'sdk-compiler',files,commands,a.timeout)
    extract=['libc.a','crt0-0.o','crt0-2.o','crt0-3.o','probe.o','probe','main.o','callee.o','variadic','argv','cells','sort-check',fallback,sorting]
    subprocess.run(['node','--input-type=module','-e',EXTRACTOR,str(disk),str(actual),json.dumps(extract)],cwd=ROOT,check=True)
    if not a.guest_only:verify_host(sdk)
    _,reloads=run_guest(out,'sdk-execute',[file('/bin/'+n,actual/n) for n in ['probe','variadic','argv','cells']],
        [('probe hello world; echo EXEC:$?','SDK_MAIN_OK hello world\nSDK_ATEXIT_OK\nEXEC:0'),
         ('variadic; echo EXEC:$?','EXEC:0'),('argv hello "x y"; echo EXEC:$?','SDK_ARGV_OK\nEXEC:0'),('cells; echo EXEC:$?','EXEC:0')],a.timeout)
    result=dict(status='guest-passed' if a.guest_only else 'passed',host_link_parity=not a.guest_only,
        units=len(rows),source_batches=batches,build=build,second_boot=reloads,
        source_refreshes=source_refreshes,
        host_dependencies=['bootstrap compiler tools','private kernel and base filesystem'],
        sdk_objects='all rebuilt from raw C inside x86 Riplix')
    (sdk/'guest-result.json').write_text(json.dumps(result,indent=2)+'\n')
    print('Raw guest libc/startup build, variadics, SDK linking and fresh-boot execution passed; '+
          ('independent host linking deferred' if a.guest_only else 'host linker parity passed'))


if __name__=='__main__':main()
