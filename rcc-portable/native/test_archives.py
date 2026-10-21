#!/usr/bin/env python3
"""Packed initializers, ordinary ar interoperability, lazy member selection,
and optional in-guest creation/linking/reboot checks. Private test disks only.
"""
import argparse
import json
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
sys.path.insert(0,str(HERE))
from test_modules import module_location,package
sys.path.insert(0,str(HERE.parent/'qbe'))
from test_x86 import run_guest,file,EXTRACTOR


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--guest',action='store_true')
    p.add_argument('--work',type=Path,default=ROOT/'build/portable-native-archives')
    p.add_argument('--output',type=Path,default=ROOT/'build/portable-native-x64')
    p.add_argument('--tools',type=Path,default=HERE.parent/'build')
    p.add_argument('--timeout',type=int,default=600)
    a=p.parse_args();w=a.work.resolve();w.mkdir(parents=True,exist_ok=True)
    source=HERE/'tests/modules';base=HERE.parent/'build';tools=w/'tools';tools.mkdir(exist_ok=True)
    for name in ['rcc-c','rcc-qbe','rcc-x64-as','rcc-x64-ld']:
        f=tools/name;f.unlink(missing_ok=True);f.symlink_to((a.tools/name).resolve())
    f=tools/'qbe';f.unlink(missing_ok=True);f.symlink_to((base/'qbe-host/qbe').resolve())
    cc=a.tools.resolve()/'rcc-native-cc';ld=a.tools.resolve()/'rcc-x64-ld';ar=a.tools.resolve()/'rcc-x64-ar'
    def run(args,**kw):return subprocess.run(list(map(str,args)),timeout=a.timeout,**kw)
    def compile(src,dst):run([cc,'--tools',tools,'-c',src,'-o',dst],check=True)
    def link(inputs,dst):
        run([ld,*inputs,'-o',dst],check=True)
        return package(dst.read_bytes())
    for name in ['main','library','peer']:compile(source/(name+'.c'),w/(name+'.o'))
    unused=w/'unused.c';unused.write_text('extern unsigned long absent(void); unsigned long counter=999UL; unsigned char unused_padding[65000]; unsigned long unused(void) { return absent(); }\n')
    compile(unused,w/'unused.o')
    members=[w/(n+'.o') for n in ['peer','library','unused']]
    run([ar,'-o',w/'libtest.a',*members],check=True)
    run([ar,'-o',w/'repeat.a',*members],check=True)
    assert (w/'libtest.a').read_bytes()==(w/'repeat.a').read_bytes()
    h,_=link([w/'main.o',w/'libtest.a'],w/'program');assert h[4]==11 and h[9:11]==(1,29)
    link([w/'main.o',w/'library.o',w/'peer.o'],w/'explicit')
    run([cc,'--tools',tools,w/'main.o',w/'libtest.a','-o',w/'driver'],check=True)
    assert (w/'driver').read_bytes()==(w/'program').read_bytes()
    # A reference with only a function-pointer initializer must extract its provider.
    init=w/'init.c';init.write_text('extern unsigned long peer(unsigned long); unsigned long (*selected_cb)(unsigned long)=peer; int main(void) { return selected_cb(2UL)!=37UL; }\n')
    # peer also needs even from library; the archive reaches a fixed point.
    compile(init,w/'init.o');link([w/'init.o',w/'libtest.a'],w/'initializer')
    # Initializer-only DATA references require selection despite having no ELF relocation.
    data=w/'data.c';data.write_text('extern unsigned long counter; unsigned long *p=&counter; int main(void) { return *p!=11UL; }\n')
    compile(data,w/'data.o');link([w/'data.o',w/'libtest.a'],w/'data')
    dense=w/'dense.c';dense.write_text('unsigned short dense[8192]={'+','.join(str(32768+i) for i in range(4096))+'};\nint main(void) { unsigned long i; for(i=0;i<4096UL;i++) if(dense[i]!=(unsigned short)(32768UL+i)) return 1; for(i=4096;i<8192UL;i++) if(dense[i]) return 2; return 0; }\n')
    compile(dense,w/'dense.o');dh,di=link([w/'dense.o'],w/'dense')
    pos,length,rows,*_=module_location((w/'dense.o').read_bytes())
    runs=[r for r in rows if r['kind']==4]
    assert len(runs)==1 and runs[0]['cells']==4096 and length<9000 and len(di)==1024
    cells={}
    for pointer,width,value,fn,_ in di:
        assert not fn
        for i in range(width):cells[(pointer&65535)+i]=(value>>(16*i))&65535
    assert [cells[i+1] for i in range(4096)]==list(range(32768,36864))
    assert dh[10]==8193 and (w/'dense.o').stat().st_size<16000
    for name in ['argv','envp']:compile(source/(name+'.c'),w/(name+'.o'));link([w/(name+'.o')],w/name)
    reference=(ROOT/'c-test/tests/core/functions/test_native_initializer_runs.c').read_text()
    wrapper=w/'runs.c'
    wrapper.write_text('static int failed; int putchar(int c) { if(c==\'N\') failed=1; return c; }\n'+reference.replace('#include <stdio.h>','').replace('int main(void)','int reference_main(void)')+'\nint main(void) { return reference_main() | failed; }\n')
    compile(wrapper,w/'runs.o');link([w/'runs.o'],w/'runs')
    # Ordinary host ar validates our writer independently, including BSD names.
    long=w/'a considerably longer object name.o';shutil.copyfile(w/'library.o',long)
    run([ar,'-o',w/'long.a',w/'peer.o',long,w/'unused.o'],check=True)
    listing=run(['ar','t',w/'long.a'],check=True,capture_output=True,text=True).stdout
    assert long.name in listing
    link([w/'main.o',w/'long.a'],w/'long')
    host=w/'host.a';host.unlink(missing_ok=True)
    # Darwin's Mach-O ranlib can discard ELF members while rebuilding its
    # index. Request an ordinary unindexed archive; our catalog ignores indexes.
    run(['ar','cqS',host,w/'peer.o',long,w/'unused.o'],check=True)
    link([w/'main.o',host],w/'host')
    # Independent wire fixture for GNU string-table names; verify it with host
    # ar as well. No reliance on which ar variant is installed on the host.
    def record(name,data):
        header=f'{name:<16}{0:<12}{0:<6}{0:<6}{"100644":<8}{len(data):<10}`\n'.encode()
        assert len(header)==60
        return header+data+(b'\n' if len(data)%2 else b'')
    gnu=b'!<arch>\n'+record('/',b'\0'*4)+record('//',(long.name+'/\n').encode())
    for name,obj in [('peer.o/',w/'peer.o'),('/0',long),('unused.o/',w/'unused.o')]:
        gnu+=record(name,obj.read_bytes())
    (w/'gnu.a').write_bytes(gnu);link([w/'main.o',w/'gnu.a'],w/'gnu')
    listing=run(['ar','t',w/'gnu.a'],check=True,capture_output=True,text=True).stdout
    if long.name not in listing: # Apple's ar treats GNU long names as literal /0.
        listing=run(['tar','-tf',w/'gnu.a'],check=True,capture_output=True,text=True).stdout
    assert long.name in listing
    # LLVM can write either BSD or GNU containers, with long names and indexes.
    llvm=shutil.which('llvm-ar')
    if not llvm:
        r=run(['xcrun','--find','llvm-ar'],capture_output=True,text=True)
        if r.returncode==0:llvm=r.stdout.strip()
    variants=['host','gnu']
    if llvm:
        for fmt in ['bsd','gnu']:
            lib=w/(fmt+'.a');lib.unlink(missing_ok=True)
            run([llvm,'--format='+fmt,'rcs',lib,w/'peer.o',long,w/'unused.o'],check=True)
            link([w/'main.o',lib],w/fmt)
            if fmt not in variants:variants.append(fmt)
    failures=0;keep=w/'keep'
    def reject(args,tool=ld):
        nonlocal failures
        keep.write_bytes(b'preserved')
        r=run([tool,*args,'-o',keep],capture_output=True)
        assert r.returncode==1 and r.stderr,(args,r.returncode,r.stderr)
        assert keep.read_bytes()==b'preserved' and not list(w.glob('keep.tmp.*'))
        failures+=1
    reject([w/'libtest.a',w/'main.o']) # standard left-to-right archive semantics
    reject([w/'main.o',w/'unused.o',w/'libtest.a'])
    reject([w/'main.o',w/'main.o'],ar)
    reject([w/'missing.o'],ar)
    for name in ['#1','__.SYMDEF_64']:
        reserved=w/name;shutil.copyfile(w/'library.o',reserved);reject([reserved],ar)
    invalid=w/'bad-main.c'
    for declaration in ['int main(int n)', 'int main(long n,char **a)',
                        'int main(int n,int a)', 'int main(int n,char **a,char **e,int extra)']:
        invalid.write_text(declaration+' { return 0; }\n')
        reject(['--tools',tools,'-c',invalid],cc)
    bad=w/'bad.a';raw=(w/'libtest.a').read_bytes()
    for blob in [b'!<thin>\n',b'!<arch>\nX',raw[:50],raw[:-1],raw[:56]+b'9999999999'+raw[66:]]:
        bad.write_bytes(blob);reject([w/'main.o',bad])
    for at,patch in [(8,b'#1/9999999999999'),(8,b'/999            '),(66,b'!'),(56,b'-1        ')]:
        b=bytearray(raw);b[at:at+len(patch)]=patch;bad.write_bytes(b);reject([w/'main.o',bad])
    rng=random.Random(1903)
    for trial in range(96):
        b=bytearray(raw)
        for _ in range(rng.randrange(1,4)):b[rng.randrange(len(b))]=rng.randrange(256)
        bad.write_bytes(b);keep.write_bytes(b'preserved')
        r=run([ld,w/'main.o',bad,'-o',keep],capture_output=True)
        assert r.returncode in (0,1),(trial,r.returncode,r.stderr)
        if r.returncode:assert keep.read_bytes()==b'preserved'
        else:package(keep.read_bytes())
    result=dict(status='passed',archive_variants=variants,rejections=failures,mutations=96,
                dense=dict(cells=4096,metadata_octets=length,object_octets=(w/'dense.o').stat().st_size,initializer_rows=len(di)))
    if a.guest:
        out=a.output.resolve()
        files=[file('/bin/'+n,base/'apps'/(n+'-x86.fxe')) for n in ['rcc-c','rcc-qbe','rcc-native-cc','rcc-x64-ld','rcc-x64-ar']]
        files += [file('/bin/qbe',ROOT/'build/guest-native-assessment/qbe-x86.fxe'),file('/bin/rcc-x64-as',ROOT/'build/guest-native-assessment/rcc-x64-as-x86.fxe')]
        files += [file('/'+n+'.o',w/(n+'.o')) for n in ['main','library','peer','unused','init','data','dense','runs']]
        files += [file('/'+n+'.c',source/(n+'.c')) for n in ['argv','envp']]
        files += [file('/dense.c',dense)]+[file('/'+n+'.a',w/(n+'.a')) for n in variants]
        commands=[
            ('rcc-x64-ar -o /tmp/libtest.a /peer.o /library.o /unused.o; echo AR:$?','AR:0'),
            ('rcc-native-cc /main.o /tmp/libtest.a -o /tmp/program; echo LINK:$?','LINK:0'),
            ('/tmp/program; echo EXEC:$?','EXEC:0'),
            ('rcc-native-cc /init.o /tmp/libtest.a -o /tmp/initializer; echo LINK:$?','LINK:0'),
            ('/tmp/initializer; echo INIT:$?','INIT:0'),
            ('rcc-native-cc /data.o /tmp/libtest.a -o /tmp/data; echo LINK:$?','LINK:0'),
            ('/tmp/data; echo DATA:$?','DATA:0'),
            ('rcc-native-cc -c /dense.c -o /tmp/dense.o; echo BUILD:$?','BUILD:0'),
            ('rcc-native-cc /tmp/dense.o -o /tmp/dense; echo LINK:$?','LINK:0'),
            ('/tmp/dense; echo DENSE:$?','DENSE:0'),
            ('rcc-native-cc -c /argv.c -o /tmp/argv.o; echo BUILD:$?','BUILD:0'),
            ('rcc-native-cc /tmp/argv.o -o /tmp/argv; echo LINK:$?','LINK:0'),
            ('/tmp/argv hello "x y"; echo ARGV:$?','ARGV:0'),
            ('/tmp/argv; echo ARGC:$?','ARGC:1'),
            ('rcc-native-cc -c /envp.c -o /tmp/envp.o; echo BUILD:$?','BUILD:0'),
            ('rcc-native-cc /tmp/envp.o -o /tmp/envp; echo LINK:$?','LINK:0'),
            ('/tmp/envp check; echo ENVP:$?','ENVP:0'),
            ('rcc-native-cc /runs.o -o /tmp/runs; echo LINK:$?','LINK:0'),
            ('/tmp/runs; echo RUNS:$?','RUNS:0'),
            ('echo preserved >/tmp/keep; rcc-x64-ld /tmp/libtest.a /main.o -o /tmp/keep; echo REJECT:$?; cat /tmp/keep','REJECT:1\npreserved'),
            ('echo preserved >/tmp/keep; rcc-x64-ar -o /tmp/keep /missing.o; echo REJECT:$?; cat /tmp/keep','REJECT:1\npreserved')]
        for fmt in variants:commands += [(f'rcc-x64-ld /main.o /{fmt}.a -o /tmp/{fmt}; echo LINK:$?','LINK:0'),(f'/tmp/{fmt}; echo EXEC:$?','EXEC:0')]
        disk,rows=run_guest(out,'archive-compiler',files,commands,a.timeout)
        actual=w/'guest';actual.mkdir(exist_ok=True);extract=['libtest.a','program','initializer','data','dense','argv','envp','runs','dense.o','argv.o','envp.o']+variants
        run(['node','--input-type=module','-e',EXTRACTOR,disk,actual,json.dumps(extract)],check=True,cwd=ROOT)
        for name in extract:
            if name in ['dense','argv','envp']:
                reference=w/(name+'.guest-link')
                run([ld,actual/(name+'.o'),'-o',reference],check=True)
                assert (actual/name).read_bytes()==reference.read_bytes(),name+' guest link differs'
            elif name.endswith('.o'):
                # QBE's equal-cost spill candidates may be ordered differently
                # by host qsort and guest heapsort. Metadata must still match.
                b=(actual/name).read_bytes();host=(w/name).read_bytes()
                at,size,*_=module_location(b);pos,length,*_=module_location(host)
                assert size==length and b[at:at+size]==host[pos:pos+length],name+' metadata differs'
            else:assert (actual/name).read_bytes()==(w/name).read_bytes(),name+' differs'
        names=['program','initializer','data','dense','runs','argv','envp']
        commands=[(n+'; echo EXEC:$?','EXEC:0') for n in names[:5]]
        commands += [('argv hello "x y"; echo ARGV:$?','ARGV:0'),('envp check; echo ENVP:$?','ENVP:0')]
        _,reloads=run_guest(out,'archive-execute',[file('/bin/'+n,actual/n) for n in names],commands,a.timeout)
        result.update(compilation=rows,second_boot=reloads)
    (w/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
