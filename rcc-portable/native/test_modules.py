#!/usr/bin/env python3
"""Separate native C modules: independent container checks, hostile metadata,
and optional complete compilation/linking/execution in freestanding x86.
"""
import argparse
import json
from pathlib import Path
import random
import struct
import subprocess
import sys

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
sys.path.insert(0,str(HERE.parent/'qbe'))
from test_x86 import run_guest,file,EXTRACTOR


def run(argv,**kwargs):
    kwargs.setdefault('timeout',30)
    return subprocess.run(list(map(str,argv)),**kwargs)


def module_location(data):
    shoff=struct.unpack_from('<Q',data,40)[0];count=struct.unpack_from('<H',data,60)[0]
    sections=[struct.unpack_from('<IIQQQQIIQQ',data,shoff+i*64) for i in range(count)]
    symtab=next(s for s in sections if s[1]==2);strings=sections[symtab[6]]
    names=data[strings[4]:strings[4]+strings[5]]
    for at in range(symtab[4],symtab[4]+symtab[5],24):
        name,info,other,index,value,size=struct.unpack_from('<IBBHQQ',data,at)
        if names[name:names.index(0,name)]==b'rcc_module_metadata':
            section=sections[index];position=section[4]+value
            assert data[position:position+8]==b'RCCMOD01' and info>>4==0
            length,records,entry=struct.unpack_from('<III',data,position+8)
            rows=[];offset=32
            for _ in range(records):
                kind,flags,size,cells,slot,dest,bits,width,mode,n,s,reserved=struct.unpack_from('<IIIIQQQIIIIQ',data,position+offset)
                name=data[position+offset+64:position+offset+64+n-1].decode()
                signature=data[position+offset+64+n:position+offset+64+n+s-1].decode()
                rows.append(dict(offset=offset,kind=kind,flags=flags,size=size,cells=cells,slot=slot,dest=dest,bits=bits,width=width,mode=mode,name=name,signature=signature))
                offset+=size
            assert offset==length
            return position,length,rows,sections,index,value
    raise AssertionError('missing embedded metadata')


def package(data):
    h=struct.unpack_from('<8s14I',data)
    assert h[0]==b'FUSEX001' and h[1]==0x31455852 and h[2]==len(data)
    nf,nh,nr,ni=h[4:8];at=64
    functions=[struct.unpack_from('<IIII',data,at+i*16) for i in range(nf)];at+=nf*16+nh*72
    fixups=[struct.unpack_from('<QQ',data,at+i*16) for i in range(nr)];at+=nr*16
    initializers=[struct.unpack_from('<IIQII',data,at+i*24) for i in range(ni)];at+=ni*24
    code=data[at:];assert len(code)==h[3]
    tokens={t for t,arity,off,reserved in functions}
    assert len(tokens)==nf and 0 not in tokens and all(t<8192 for t in tokens)
    assert any(t==h[8] and arity==1 for t,arity,_,_ in functions)
    assert all(arity<=64 and off<len(code) and not reserved for _,arity,off,reserved in functions)
    assert all(off%8==0 and off+8<=len(code) and add<len(code) for off,add in fixups)
    assert h[9] in (1,32) and 0<h[10]<=(64000 if h[9]==1 else 1024000) and h[11]==0
    assert h[12]%8==0 and h[12]+nh*8<=len(code) and h[13:]==(0,0)
    end=0
    for pointer,width,value,function,reserved in initializers:
        bank,offset=pointer>>16,pointer&65535
        index=(bank-h[9])*64000+offset
        assert bank>=h[9] and offset<64000 and 1<=width<=4 and index>=end and index>0 and index+width<=h[10]
        assert reserved==0 and function in (0,1)
        if function:assert width==2 and value^0x80000000 in tokens
        end=index+width
    assert all((at+off)%16==0 for _,_,off,_ in functions)
    return h,initializers


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--guest',action='store_true')
    p.add_argument('--skip-build',action='store_true',help='reuse the private kernel boot seed')
    p.add_argument('--output',type=Path,default=ROOT/'build/portable-native-x64')
    p.add_argument('--work',type=Path,default=ROOT/'build/portable-native-modules')
    p.add_argument('--linker',type=Path,default=HERE.parent/'build/rcc-x64-ld')
    p.add_argument('--timeout',type=int,default=480)
    a=p.parse_args();work=a.work.resolve();work.mkdir(parents=True,exist_ok=True)
    tools=work/'tools';tools.mkdir(exist_ok=True)
    for name,path in [('rcc-c',HERE.parent/'build/rcc-c'),('rcc-qbe',HERE.parent/'build/rcc-qbe'),
                      ('qbe',HERE.parent/'build/qbe-host/qbe'),('rcc-x64-as',HERE.parent/'build/rcc-x64-as'),('rcc-x64-ld',a.linker)]:
        target=tools/name
        target.unlink(missing_ok=True);target.symlink_to(path.resolve())
    cc=HERE.parent/'build/rcc-native-cc';ld=a.linker.resolve();source=HERE/'tests/modules'
    def compile(path,dest):
        run([cc,'--tools',tools,'-c',path,'-o',dest],check=True)
    def link(objects,dest):
        run([ld,*objects,'-o',dest],check=True)
        return package(dest.read_bytes())
    for name in ['main','library','peer','banks']:compile(source/(name+'.c'),work/(name+'.o'))
    objects=[work/(name+'.o') for name in ['main','library','peer']]
    h,initializers=link(objects,work/'modules')
    assert h[4]==11 and h[9:11]==(1,29)
    assert sum(v[3] for v in initializers)==1
    assert any((value >> (16*i)) & 65535 == 4660 for _,width,value,_,_ in initializers for i in range(width))
    reverse,_=link(objects[::-1],work/'reverse');assert reverse[8]!=h[8]
    banks,_=link([work/'banks.o'],work/'banks');assert banks[9]==32 and banks[10]>128000
    run([cc,'--tools',tools,*[source/(name+'.c') for name in ['main','library','peer']],'-o',work/'combined'],check=True)
    assert (work/'combined').read_bytes()==(work/'modules').read_bytes()
    failures=0;bad=work/'bad.o';out=work/'preserved'
    def reject(objects,reason):
        nonlocal failures
        out.write_bytes(b'preserved')
        r=run([ld,*objects,'-o',out],capture_output=True)
        assert r.returncode==1 and r.stderr,reason+': '+repr(r)
        assert out.read_bytes()==b'preserved' and not list(work.glob('preserved.tmp.*')),reason+' damaged output'
        failures+=1
    reject(objects[1:],'no entry');reject([*objects,objects[1]],'duplicate C definitions')
    consumer=work/'consumer.c';provider=work/'provider.c'
    consumer.write_text('extern unsigned long f(unsigned long); int main(void) { return f(7UL)!=7UL; }\n')
    provider.write_text('double f(double x) { return x; }\n')
    compile(consumer,work/'consumer.o');compile(provider,work/'provider.o')
    reject([work/'consumer.o',work/'provider.o'],'function ABI mismatch')
    reject([work/'consumer.o'],'undefined function')
    provider.write_text('unsigned long f=7UL;\n');compile(provider,work/'provider.o')
    reject([work/'consumer.o',work/'provider.o'],'function/object kind mismatch')
    consumer.write_text('extern unsigned long g; int main(void) { return g!=7UL; }\n')
    provider.write_text('double g=7.0;\n')
    compile(consumer,work/'consumer.o');compile(provider,work/'provider.o')
    reject([work/'consumer.o',work/'provider.o'],'global scalar ABI mismatch')
    consumer.write_text('extern int g[3]; int main(void) { return g[0]; }\n')
    provider.write_text('int g[4];\n')
    compile(consumer,work/'consumer.o');compile(provider,work/'provider.o')
    reject([work/'consumer.o',work/'provider.o'],'global extent mismatch')
    # Explicit incomplete array, null pointer, cyclic data addresses and unused
    # declarations are valid; initializers are independent of source/link order.
    consumer.write_text('extern int unused(void); extern int a[]; extern int *p; extern void *left,*right; int main(void) { return a[0]!=9 || p!=0 || *(void**)left!=&left || *(void**)right!=&right; }\n')
    provider.write_text('int a[2]={9,11}; int *p=0; extern void *right; void *left=&right; void *right=&left;\n')
    compile(consumer,work/'consumer.o');compile(provider,work/'provider.o')
    link([work/'consumer.o',work/'provider.o'],work/'incomplete')
    # Aggregate results use pointer carriers but retain their structural return
    # layout in the module signature. Equal total size is insufficient.
    consumer.write_text('struct R {unsigned long long a; unsigned long b;}; struct R make(void); int main(void) {struct R r=make();return r.a!=7 || r.b!=9;}\n')
    provider.write_text('struct R {unsigned long long a; unsigned long b;}; struct R make(void) {struct R r={7,9};return r;}\n')
    compile(consumer,work/'consumer.o');compile(provider,work/'provider.o')
    link([work/'consumer.o',work/'provider.o'],work/'aggregate')
    provider.write_text('struct R {unsigned long a; unsigned long long b;}; struct R make(void) {struct R r={7,9};return r;}\n')
    compile(provider,work/'provider.o')
    reject([work/'consumer.o',work/'provider.o'],'aggregate result layout mismatch')
    changed=work/'changed.c'
    changed.write_text('unsigned long even(unsigned long); unsigned long peer(unsigned long n) { return n*3UL+32UL; } unsigned long odd(unsigned long n) { return n ? even(n-1UL) : 0UL; }\n')
    compile(changed,work/'changed.o');link([*objects[:2],work/'changed.o'],work/'changed')
    base=objects[0].read_bytes();position,length,records,ss,index,value=module_location(base)
    first=records[0]['offset'];named=next(r for r in records if r['name']=='helper')
    changes=[(0,b'BADMOD01'),(8,struct.pack('<I',31)),(8,struct.pack('<I',length+8)),
             (12,struct.pack('<I',999999)),(16,struct.pack('<I',999999)),(20,b'\1'),
             (first,struct.pack('<I',99)),(first+4,struct.pack('<I',128)),
             (first+8,struct.pack('<I',65)),(first+12,struct.pack('<I',65)),
             (first+16,b'\1'),(first+28,b'\1'),(first+48,struct.pack('<I',999999)),
             (first+56,b'\1'),(first+64,b'1'),
             (named['offset']+4,struct.pack('<I',1)), # local definition claimed public
             (named['offset']+64+len('helper')+1,b'?')]
    for at,patch in changes:
        b=bytearray(base);b[position+at:position+at+len(patch)]=patch;bad.write_bytes(b)
        reject([bad,*objects[1:]],f'invalid metadata at {at}')
    library=objects[1].read_bytes();pos,size,rows,_,_,_=module_location(library)
    init=next(r for r in rows if r['kind']==3)
    for at,patch in [(init['offset']+24,struct.pack('<Q',999999)),(init['offset']+40,struct.pack('<I',5)),
                     (init['offset']+44,struct.pack('<I',3))]:
        b=bytearray(library);b[pos+at:pos+at+len(patch)]=patch;bad.write_bytes(b)
        reject([objects[0],bad,objects[2]],'invalid initializer')
    sample=next(r for r in rows if r['kind']==4 and r['name']=='counter');b=bytearray(library)
    b[pos+sample['offset']+64:pos+sample['offset']+64+8]=b'samples\0'
    struct.pack_into('<Q',b,pos+sample['offset']+24,1);bad.write_bytes(b)
    reject([objects[0],bad,objects[2]],'overlapping logical initializers')
    for at,patch in [(4,struct.pack('<I',1)),(12,struct.pack('<I',0)),(12,struct.pack('<I',999999)),
                     (24,struct.pack('<Q',999999)),(32,b'\1'),(40,b'\1'),(44,b'\1')]:
        b=bytearray(library);b[pos+sample['offset']+at:pos+sample['offset']+at+len(patch)]=patch
        bad.write_bytes(b);reject([objects[0],bad,objects[2]],'invalid packed cell run')
    # An ELF relocation may not rewrite the metadata which was just validated.
    b=bytearray(base);rel=next(s for s in ss if s[1]==4 and s[5]);rpos=rel[4]
    shoff=struct.unpack_from('<Q',b,40)[0];rindex=ss.index(rel)
    struct.pack_into('<I',b,shoff+rindex*64+44,index)
    struct.pack_into('<Q',b,rpos,value+first+16)
    bits=struct.unpack_from('<Q',b,rpos+8)[0];struct.pack_into('<Q',b,rpos+8,(bits&~0xffffffff)|1)
    bad.write_bytes(b);reject([bad,*objects[1:]],'ELF relocation overlaps metadata')
    rng=random.Random(1701)
    for trial in range(128):
        b=bytearray(base)
        for _ in range(rng.randrange(1,4)):b[position+rng.randrange(length)]=rng.randrange(256)
        bad.write_bytes(b);out.write_bytes(b'preserved')
        r=run([ld,bad,*objects[1:],'-o',out],capture_output=True)
        assert r.returncode in (0,1),(trial,r.returncode,r.stderr)
        if r.returncode:assert out.read_bytes()==b'preserved'
        else:package(out.read_bytes())
    result=dict(status='passed',rejections=failures,mutations=128)
    if a.guest:
        outdir=a.output.resolve();outdir.mkdir(parents=True,exist_ok=True)
        if not a.skip_build:
            run(['python3',ROOT/'tools/x86_64/boot.py','build','--font','rom','--output',outdir],check=True,timeout=a.timeout)
        files=[file('/bin/'+name,path) for name,path in [
            ('rcc-c',HERE.parent/'build/apps/rcc-c-x86.fxe'),('rcc-qbe',HERE.parent/'build/apps/rcc-qbe-x86.fxe'),
            ('qbe',ROOT/'build/guest-native-assessment/qbe-x86.fxe'),('rcc-x64-as',ROOT/'build/guest-native-assessment/rcc-x64-as-x86.fxe'),
            ('rcc-x64-ld',HERE.parent/'build/apps/rcc-x64-ld-x86.fxe'),('rcc-native-cc',HERE.parent/'build/apps/rcc-native-cc-x86.fxe')]]
        files += [file('/'+f.name,f) for f in source.glob('*.[ch]')]
        files += [file('/changed.c',changed),file('/bin/incomplete',work/'incomplete')]
        for name in ['main','library','peer','banks']:files.append(file('/host-'+name+'.o',work/(name+'.o')))
        commands=[]
        for name in ['main','library','peer','banks']:
            commands.append((f'rcc-native-cc -c /{name}.c -o /tmp/{name}.o; echo COMPILE:$?','COMPILE:0'))
        commands += [
            ('rcc-native-cc /tmp/main.o /tmp/library.o /tmp/peer.o -o /tmp/modules; echo LINK:$?','LINK:0'),
            ('/tmp/modules; echo MODULES:$?','MODULES:0'),
            ('rcc-x64-ld /tmp/peer.o /tmp/library.o /tmp/main.o -o /tmp/reverse; echo LINK:$?','LINK:0'),
            ('/tmp/reverse; echo REVERSE:$?','REVERSE:0'),
            ('rcc-native-cc /tmp/banks.o -o /tmp/banks; echo LINK:$?','LINK:0'),
            ('/tmp/banks; echo BANKS:$?','BANKS:0'),
            ('/tmp/modules; echo RESET:$?','RESET:0'),
            ('rcc-native-cc -c /changed.c -o /tmp/changed.o; echo REBUILD:$?','REBUILD:0'),
            ('rcc-native-cc /tmp/main.o /tmp/library.o /tmp/changed.o -o /tmp/changed; echo LINK:$?','LINK:0'),
            ('/tmp/changed; echo CHANGED:$?','CHANGED:4'),
            ('rcc-x64-ld /host-main.o /tmp/library.o /host-peer.o -o /tmp/mixed; echo LINK:$?','LINK:0'),
            ('/tmp/mixed; echo MIXED:$?','MIXED:0'),
            ('/bin/incomplete; echo INITIALIZERS:$?','INITIALIZERS:0'),
            ('rcc-native-cc /main.c /library.c /peer.c -o /tmp/combined; echo COMBINED:$?','COMBINED:0'),
            ('/tmp/combined; echo EXEC:$?','EXEC:0'),
            ('echo preserved >/tmp/keep; rcc-native-cc /tmp/library.o /tmp/peer.o -o /tmp/keep; echo REJECT:$?; cat /tmp/keep','REJECT:1\npreserved')]
        root,commands=run_guest(outdir,'module-compiler',files,commands,a.timeout)
        extracts=[name+'.o' for name in ['main','library','peer','banks','changed']]+['modules','reverse','banks','mixed','combined','changed']
        actual=work/'guest';actual.mkdir(exist_ok=True)
        run(['node','--input-type=module','-e',EXTRACTOR,root,actual,json.dumps(extracts)],cwd=ROOT,check=True)
        for name in extracts:
            expected=work/('modules' if name=='mixed' else name)
            assert (actual/name).read_bytes()==expected.read_bytes(),name+' host/guest differs'
        _,reloads=run_guest(outdir,'module-execute',[file('/bin/'+name,actual/name) for name in ['modules','reverse','banks','mixed']],
            [(name+'; echo EXEC:$?','EXEC:0') for name in ['modules','reverse','banks','mixed']],a.timeout)
        result.update(compilation=commands,second_boot=reloads)
    (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(f'Separate native modules, globals, callbacks and bank layout: {failures} rejections and 128 mutations passed')


if __name__=='__main__':main()
