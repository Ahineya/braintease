#!/usr/bin/env python3
"""HOST bootstrap packager for QBE continuation probes, using existing RXE1.

This is intentionally not a guest linker or a general C SDK packager. It binds
only the checked native helpers and scalar function metadata emitted by rcc-qbe.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
sys.path.insert(0,str(ROOT/'tools/x86_64'))
from app_link import flatten, TAG
from boot import elf_linker
HELPERS = dict(rcc_native_poll=0,rcc_native_load=2,rcc_native_store=3,rcc_native_gep=2,
    rcc_native_image_token=1,rcc_native_process_frame=1,rcc_native_process_pc=0,
    rcc_native_process_arg=1,rcc_native_process_slot=1,rcc_native_process_set_pc=1,
    rcc_native_process_save=2,rcc_native_process_argument=2,rcc_native_process_call=3,
    rcc_native_process_return=1,rcc_native_process_checkpoint=0,rcc_native_process_request=4,
    rcc_native_process_bad_pc=0)


def undefined(path):
    data=path.read_bytes()
    if data[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<HH',data,16)!=(1,62):
        raise ValueError('expected little-endian x86_64 ELF relocatable')
    at=struct.unpack_from('<Q',data,40)[0]; size,count=struct.unpack_from('<HH',data,58)
    sections=[struct.unpack_from('<IIQQQQIIQQ',data,at+i*size) for i in range(count)]
    names=set()
    for s in sections:
        if s[1]!=2: continue
        strings=sections[s[6]]; table=data[strings[4]:strings[4]+strings[5]]
        for pos in range(s[4],s[4]+s[5],s[9]):
            name,info,_,index,_,_=struct.unpack_from('<IBBHQQ',data,pos)
            if name and not index and info>>4 in (1,2): names.add(table[name:table.index(0,name)].decode())
    return sorted(names)


def package(source,obj,output,assembler=None):
    text=source.read_text()
    entries=re.findall(r'^# rcc-entry (\d+)$',text,re.M)
    rows=re.findall(r'^# rcc-function (\d+) (\d+) ([A-Za-z_][A-Za-z_0-9]*)$',text,re.M)
    functions=[(int(token),int(arity),symbol) for token,arity,symbol in rows]
    if len(entries)!=1 or not functions or len({f[0] for f in functions})!=len(functions) or len({f[2] for f in functions})!=len(functions):
        raise ValueError('missing/duplicate continuation metadata')
    entry=int(entries[0])
    if any(not 0<t<8192 or not 0<=a<=64 for t,a,_ in functions) or not any(t==entry and a==1 for t,a,_ in functions):
        raise ValueError('invalid continuation token/arity')
    names=undefined(obj)
    if any(n not in HELPERS for n in names): raise ValueError('unsupported runtime imports: '+repr(names))
    output.parent.mkdir(parents=True,exist_ok=True)
    assembler=assembler or HERE.parent/'build/rcc-x64-as'
    with tempfile.TemporaryDirectory(prefix='.qbe-package-',dir=output.parent) as folder:
        work=Path(folder); glue=work/'helpers.s'; bindings=work/'helpers.o'; elf=work/'application.elf'
        lines=['.text']
        for i,name in enumerate(names): lines += ['.globl '+name,name+':',f'jmp *riplix_helpers+{i*8}(%rip)']
        lines += ['.data','.balign 8','.globl riplix_helpers','riplix_helpers:',f'.zero {max(1,len(names))*8}']
        glue.write_text('\n'.join(lines)+'\n')
        subprocess.run([str(assembler.resolve()),str(glue),'-o',str(bindings)],check=True)
        subprocess.run([*elf_linker(),'-pie','--no-dynamic-linker','-Bsymbolic','--no-undefined','-e','rcc_user_entry',
                        '-o',str(elf),str(obj),str(bindings)],check=True)
        code,symbols,relocations=flatten(elf)
        # RXE1's tables need only eight-byte alignment, while linked ELF data
        # can require a page-aligned load bias (e.g. aligned SSE constants).
        # Preserve that bias inside the opaque code blob; no loader ABI change.
        metadata=64+len(functions)*16+len(names)*72+len(relocations)*16
        padding=(-metadata)%4096
        if len(code)+padding>16*1024*1024:raise ValueError('aligned code exceeds RXE1 capacity')
        code=bytes(padding)+code
        symbols={name:value+padding for name,value in symbols.items()}
        relocations=[(offset+padding,addend+padding) for offset,addend in relocations]
        function_table=b''.join(struct.pack('<IIII',token,arity,symbols[symbol],0) for token,arity,symbol in functions)
        helpers=b''.join(struct.pack('<64sII',name.encode(),HELPERS[name],0) for name in names)
        relocs=b''.join(struct.pack('<QQ',*r) for r in relocations)
        body=function_table+helpers+relocs+code
        header=struct.pack('<8s14I',b'FUSEX001',TAG,64+len(body),len(code),len(functions),len(names),len(relocations),0,
                           entry,1,1,0,symbols['riplix_helpers'],0,0)
        staged=work/'program'; staged.write_bytes(header+body); staged.replace(output)
    output.with_suffix('.json').write_text(json.dumps(dict(format='RXE1',architecture='x86_64',
        bootstrap=True,functions=len(functions),code_octets=len(code),code_padding_octets=padding,
        package_octets=64+len(body),helpers=names),indent=2)+'\n')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path);p.add_argument('object',type=Path);p.add_argument('-o',type=Path,required=True)
    a=p.parse_args();package(a.source,a.object,a.o);print(a.o)


if __name__=='__main__':main()
