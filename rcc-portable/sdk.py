#!/usr/bin/env python3
"""Install C16 headers and compact, portable objects from the existing SDK.

This is a bootstrap format conversion, not guest compilation. The resulting
SDK is consumed directly by rcc-ld with no host process or service.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parent
BANK=64000
CHUNK=256
PAYLOAD=254
RNONE,RJAL,RCBANK,RCOFF,RDBANK,RDOFF,RDATA=range(7)
TNONE,TLOCALCODE,TLOCALDATA,TEXTERNAL=range(4)
KINDS={'absolute':RJAL,'code_bank':RCBANK,'code_offset':RCOFF,
       'data_bank':RDBANK,'data_offset':RDOFF,'data':RDATA}


def u32(n):return struct.pack('<I',n)
def string(s):
    b=s.encode('ascii')
    return u32(len(b))+b


def convert(obj):
    code=obj['instructions'];labels={name:value['absolute_address']//4 for name,value in obj['labels'].items()}
    data_labels=obj['data_labels'];refs={int(k):v for k,v in obj['unresolved_references'].items()}
    externs=[];external={};records=[];positions={}
    # record is op,a,b,c,reloc,target,value,addend. Local code addresses are
    # converted after every original instruction has a packed address.
    def external_id(name):
        if name not in external:external[name]=len(externs);externs.append(name)
        return external[name]
    def target(name,kind):
        addend=0
        if kind in [RDBANK,RDOFF] and '+' in name:
            name,n=name.split('+',1);addend=int(n)
        if name in labels:return TLOCALCODE,labels[name],addend
        if name in data_labels:return TLOCALDATA,data_labels[name],addend
        return TEXTERNAL,external_id(name),addend
    def room(count):
        if len(records)%CHUNK+count<=PAYLOAD:return
        while len(records)%CHUNK<PAYLOAD:records.append([0x42,0,0,0,0,0,0,0])
        following=(len(records)//CHUNK+1)*CHUNK
        # bridge address already names a packed position, unlike source refs.
        records.append([0x13,0,0,0,RJAL,-TLOCALCODE,following,0])
        records.append([0x42,0,0,0,0,0,0,0])
    for index,inst in enumerate(code):
        op=inst['opcode'];a,b,c=inst['word1'],inst['word2'],inst['word3'];ref=refs.get(index)
        positions[index]=len(records)
        if 0x15<=op<=0x18:
            if ref:
                if ref['ref_type']!='branch':raise ValueError('branch relocation kind')
                tk,tv,add=target(ref['label'],RJAL)
            else:
                tv=index+(c if c<32768 else c-65536)
                if a==b and op in [0x16,0x17]:room(1);records.append([0x42,0,0,0,0,0,0,0]);continue
                if not 0<=tv<len(code):raise ValueError('numeric branch outside object')
                tk,add=TLOCALCODE,0
            if a==b:
                room(1)
                records.append([0x42,0,0,0,0,0,0,0] if op in [0x16,0x17] else [0x13,0,0,0,RJAL,tk,tv,add])
            else:
                room(2);inverse={0x15:0x16,0x16:0x15,0x17:0x18,0x18:0x17}[op]
                records.append([inverse,a,b,2,0,0,0,0]);records.append([0x13,0,0,0,RJAL,tk,tv,add])
        else:
            kind=RNONE;tk=TNONE;tv=add=0
            if ref:
                kind=KINDS[ref['ref_type']];tk,tv,add=target(ref['label'],kind)
                if kind==RJAL and op!=0x13:raise ValueError('absolute relocation outside JAL')
            elif op==0x13:
                if b or c>=len(code):raise ValueError('unrelocated nonlocal JAL')
                kind=RJAL;tk=TLOCALCODE;tv=c
            room(1);records.append([op,a,b,c,kind,tk,tv,add])
    positions[len(code)]=len(records)
    if records:
        while len(records)%CHUNK:records.append([0x42,0,0,0,0,0,0,0])
    for r in records:
        if r[5]==TLOCALCODE:r[6]=positions[r[6]]
        elif r[5]==-TLOCALCODE:r[5]=TLOCALCODE
    defs=[]
    def public(name):return not name.startswith(('L_','.','__rcc_local_','__rcp_','__gp_'))
    for name,value in sorted(labels.items()):
        if public(name):defs.append((name,0,positions[value]))
    for name,value in sorted(data_labels.items()):
        if public(name):defs.append((name,1,value))
    bias=next((v for n,v in data_labels.items() if n.startswith('__gp_')),0)
    data=bytes(bias)+bytes(obj['data'])
    # Preserve trailing reservations even in legacy objects with an origin.
    size=max(len(data),max(data_labels.values(),default=0))
    data+=bytes(size-len(data))
    header=b'RPO1'+struct.pack('<6I',1,len(records),len(data),len(defs),len(externs),int(bool((obj.get('rcc') or {}).get('banked_data'))))
    header+=b''.join(string(n)+struct.pack('<HI',kind,offset) for n,kind,offset in defs)
    header+=b''.join(map(string,externs))
    return header+b''.join(struct.pack('<6H2I',*r[:6],r[6],r[7]&0xffffffff) for r in records)+data


def archive(objects):
    return b'RPA1'+u32(len(objects))+b''.join(u32(len(o))+o for o in objects)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
    p.add_argument('--output',type=Path,default=HERE/'build/sdk')
    p.add_argument('--float',action='store_true',dest='floating')
    args=p.parse_args()
    make=['make','--no-print-directory','-s','-C',str(ROOT/'ports/fuzix'),f'BUILD={args.build.resolve()}']
    subprocess.run([*make,'rfl-sdk',*(['float-sdk'] if args.floating else [])],check=True)
    sdk=json.loads(subprocess.check_output([*make,'rfl-sdk-info'],text=True));libraries=list(map(Path,sdk['libraries']))
    if args.floating:
        floats=json.loads(subprocess.check_output([*make,'float-sdk-info'],text=True))
        libraries=[p for p in libraries if p!=args.build.resolve()/'libc/format.pobj']+list(map(Path,floats['libraries']))
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.sdk-',dir=output) as tmp:
        work=Path(tmp)
        (work/'entry.rpo').write_bytes(convert(json.loads(Path(sdk['entry']).read_text())))
        (work/'libc.rpa').write_bytes(archive([convert(json.loads(path.read_text())) for path in libraries]))
        (work/'bare.rpo').write_bytes(convert(json.loads((ROOT/'runtime/crt0.pobj').read_text())))
        runtime=json.loads((ROOT/'runtime/libruntime.par').read_text())
        (work/'runtime.rpa').write_bytes(archive([convert(o['object']) for o in runtime['objects']]))
        # Preserve the SDK's relative include paths inside a self-contained tree.
        for relative in ['runtime/include','ports/fuzix/ripple/include','ports/fuzix/upstream/Library/include','ports/fuzix/upstream/Kernel/include']:
            shutil.copytree(ROOT/relative,work/'tree'/relative)
        shutil.copy2(ROOT/'ports/fuzix/ripple/exec-args.h',work/'tree/ports/fuzix/ripple/exec-args.h')
        # A short -I/include path uses forwarding headers into that tree.
        include=work/'include';include.mkdir()
        paths=[ROOT/'ports/fuzix/ripple/include',ROOT/'ports/fuzix/upstream/Library/include',ROOT/'runtime/include']
        for base in paths:
            for header in base.rglob('*.h'):
                dest=include/header.relative_to(base)
                if dest.exists():continue
                dest.parent.mkdir(parents=True,exist_ok=True)
                target=work/'tree'/header.relative_to(ROOT)
                dest.write_text('#include "'+os.path.relpath(target,dest.parent)+'"\n')
        for path in ['entry.rpo','libc.rpa','bare.rpo','runtime.rpa']:
            os.replace(work/path,output/path)
        for name in ['tree','include']:
            if (output/name).exists():shutil.rmtree(output/name)
            shutil.move(str(work/name),str(output/name))
    print(output)

if __name__=='__main__':main()
