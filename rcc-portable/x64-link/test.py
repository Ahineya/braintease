#!/usr/bin/env python3
"""Independent RXE1/relocation oracle and malformed ELF/atomic-output tests."""
import argparse
from pathlib import Path
import random
import struct
import subprocess
import tempfile

HERE=Path(__file__).resolve().parent


def sections(data):
    at=struct.unpack_from('<Q',data,40)[0]
    count=struct.unpack_from('<H',data,60)[0]
    return at,[struct.unpack_from('<IIQQQQIIQQ',data,at+64*i) for i in range(count)]


def image(data):
    h=struct.unpack_from('<8s14I',data)
    assert h[0]==b'FUSEX001' and h[1]==0x31455852 and h[2]==len(data)
    nf,nh,nr,ni=h[4:8]
    assert ni==0 and h[9:12]==(1,1,0) and h[13:]==(0,0)
    at=64
    funcs=[struct.unpack_from('<IIII',data,at+16*i) for i in range(nf)];at+=16*nf
    helpers=[struct.unpack_from('<64sII',data,at+72*i) for i in range(nh)];at+=72*nh
    relocs=[struct.unpack_from('<QQ',data,at+16*i) for i in range(nr)];at+=16*nr
    assert len(data)-at==h[3] and h[12]%8==0
    assert all(0<t<8192 and a<=64 and off<h[3] and not reserved for t,a,off,reserved in funcs)
    assert len(set(t for t,_,_,_ in funcs))==nf
    assert any(t==h[8] and a==1 for t,a,_,_ in funcs)
    assert all(off%8==0 and off+8<=h[3] and add<h[3] for off,add in relocs)
    assert h[12]+nh*8<=h[3]
    return h,at,data[at:],funcs,helpers,relocs


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool',type=Path,default=HERE.parent/'build/rcc-x64-ld')
    p.add_argument('--assembler',type=Path,default=HERE.parent/'build/rcc-x64-as')
    a=p.parse_args();tool=str(a.tool.resolve());assembler=str(a.assembler.resolve())
    with tempfile.TemporaryDirectory(prefix='rcc-link-test-') as folder:
        w=Path(folder);out=w/'program';meta=w/'program.ssa'
        metadata='# rcc-entry 1\n# rcc-function 1 1 entry\n'
        meta.write_text(metadata)
        def assemble(name,source):
            path=w/(name+'.s');obj=w/(name+'.o');path.write_text(source)
            subprocess.run([assembler,str(path),'-o',str(obj)],check=True)
            return obj
        first=assemble('first','.text\n.globl entry\nentry:\ncall other\nleaq item(%rip), %rax\nmovl $7, item(%rip)\ncall rcc_native_poll\nret\n.data\n.balign 16\nitem: .quad other+3\n.bss\n.balign 32\n.zero 32\n.section .rodata\n.balign 4096\n.quad 0x123456789abcdef0\n')
        second=assemble('second','.text\n.globl other\nother: ret\n')
        command=[tool,'--metadata',str(meta),str(first),str(second),'-o',str(out)]
        subprocess.run(command,check=True)
        good=out.read_bytes();h,start,code,funcs,helpers,relocs=image(good)
        entry=funcs[0][2]
        assert (start+entry)%16==0 and out.stat().st_mode&0o111
        assert code[entry]==0xe8
        other=entry+5+struct.unpack_from('<i',code,entry+1)[0]
        assert code[other]==0xc3
        assert code[entry+5:entry+8]==b'\x48\x8d\x05'
        item=entry+12+struct.unpack_from('<i',code,entry+8)[0]
        assert (start+item)%16==0
        assert relocs==[(item,other+3)] and code[item:item+8]==bytes(8)
        # The immediate following the RIP displacement is part of P+A.
        assert entry+22+struct.unpack_from('<i',code,entry+14)[0]==item
        assert helpers==[(b'rcc_native_poll'.ljust(64,b'\0'),0,0)]
        trampoline=entry+27+struct.unpack_from('<i',code,entry+23)[0]
        assert code[trampoline:trampoline+2]==b'\xff\x25'
        assert trampoline+6+struct.unpack_from('<i',code,trampoline+2)[0]==h[12]
        pattern=struct.pack('<Q',0x123456789abcdef0)
        constant=code.index(pattern)
        assert (start+constant)%4096==0
        assert not any(code[item+16:item+48]) # BSS starts after 16-aligned data.
        assert code[h[12]:h[12]+8]==bytes(8)

        # Absolute constants may use narrow relocations; relocatable native
        # addresses may not be truncated before the loader chooses a base.
        constants=assemble('constants','.text\n.globl entry\nentry: ret\n.data\n.quad constant\n.long constant\n.short constant\n.byte constant\n')
        constant_object=assemble('definition','.data\n.globl constant\nconstant: .zero 1\n')
        data=bytearray(constant_object.read_bytes());_,ss=sections(data)
        st=next(s for s in ss if s[1]==2)
        sym=st[4]+24
        struct.pack_into('<HQQ',data,sym+6,65521,42,0);constant_object.write_bytes(data)
        subprocess.run([tool,'--metadata',str(meta),str(constants),str(constant_object),'-o',str(out)],check=True)
        _,_,constant_code,_,_,fix=image(out.read_bytes())
        assert not fix and struct.pack('<QIHB',42,42,42,42) in constant_code

        failures=0
        def reject(args,reason):
            nonlocal failures
            out.write_bytes(b'preserve')
            result=subprocess.run([tool,*map(str,args),'-o',str(out)],capture_output=True)
            assert result.returncode==1 and result.stderr,reason+': '+repr(result)
            assert out.read_bytes()==b'preserve',reason+' damaged output'
            assert not list(w.glob('program.tmp.*')),reason+' leaked temporary'
            failures+=1
        args=['--metadata',meta,first,second]
        reject(['--metadata',meta,first],'undefined')
        reject([*args,second],'duplicate definition')
        reject(['--metadata',meta],'no objects')
        broken=w/'broken.o';base=first.read_bytes();shoff,ss=sections(base)
        symtab=next(i for i,s in enumerate(ss) if s[1]==2)
        rela=next(i for i,s in enumerate(ss) if s[1]==4 and s[5])
        patches=[(0,b'NOTELF'),(4,b'\1'),(5,b'\2'),(16,struct.pack('<H',2)),
                 (18,struct.pack('<H',3)),(40,struct.pack('<Q',2**64-1)),
                 (58,struct.pack('<H',63)),(60,b'\0\0'),(62,b'\xff\xff'),
                 (shoff+64+8,struct.pack('<Q',0x402)), # TLS
                 (shoff+64+48,struct.pack('<Q',3)),
                 (shoff+64+48,struct.pack('<Q',8192)),
                 (shoff+64+24,struct.pack('<Q',len(base))),
                 (shoff+symtab*64+56,struct.pack('<Q',23)),
                 (shoff+symtab*64+40,struct.pack('<I',999999)),
                 (shoff+symtab*64+44,struct.pack('<I',999999)),
                 (ss[symtab][4]+24,struct.pack('<I',999999)),
                 (ss[symtab][4]+24+4,b'\x20'), # weak
                 (ss[symtab][4]+24+6,b'\xf2\xff'), # COMMON
                 (ss[symtab][4]+24+8,struct.pack('<Q',2**64-1)),
                 (shoff+rela*64+44,struct.pack('<I',0)),
                 (shoff+rela*64+40,struct.pack('<I',0)),
                 (ss[rela][4],struct.pack('<Q',2**64-1)),
                 (ss[rela][4]+8,struct.pack('<Q',(999999<<32)|4)),
                 (ss[rela][4]+8,struct.pack('<Q',9)), # GOTPCREL
                 (ss[rela][4]+16,struct.pack('<Q',0x80000000)),
                 (ss[rela][4]+24,struct.pack('<Q',1))] # overlap
        for at,value in patches:
            b=bytearray(base);b[at:at+len(value)]=value;broken.write_bytes(b)
            reject(['--metadata',meta,broken,second],f'malformed ELF at {at}')
        for length in [0,63,len(base)-1]:
            broken.write_bytes(base[:length]);reject(['--metadata',meta,broken,second],'truncated')
        bad_meta=['',metadata*2,'# rcc-entry 2\n# rcc-function 1 1 entry\n',
                  '# rcc-entry 1\n# rcc-function 1 0 entry\n',
                  '# rcc-entry 1\n# rcc-function 1 1 missing\n',
                  '# rcc-entry 8192\n# rcc-function 8192 1 entry\n',
                  '# rcc-entry 18446744073709551617\n# rcc-function 1 1 entry\n',
                  metadata+'# rcc-function 2 1 entry\n',metadata+'# rcc-function 1 1 other\n',
                  '# rcc-entry 1junk\n# rcc-function 1 1 entry\n',
                  '# rcc-entry 1\n# rcc-function 1 65 entry\n',metadata+'# rcc-unknown 1\n',
                  '# rcc-entry 1\n# rcc-function 1 1 entry junk\n']
        for content in bad_meta:
            meta.write_text(content);reject(args,'malformed metadata')
        for content in [metadata+'# rcc-function 2 1 '+('x'*1024)+'\n',metadata+'\0hidden\n',
                        '# rcc-entry 1\n# rcc-function 1 1 éntry\n']:
            meta.write_text(content);reject(args,'overlong/non-ASCII/NUL metadata')
        meta.write_text(metadata+'# ordinary QBE text '+('x'*2048))
        subprocess.run(command,check=True)
        meta.write_text(metadata)
        for text in ['.long other','.short other','.byte other','.byte 0\n.quad other']:
            bad=assemble('bad-reloc','.text\n.globl entry\nentry: ret\n.data\n'+text+'\n')
            reject(['--metadata',meta,bad,second],'unsupported base relocation')
        reserved=assemble('reserved','.text\n.globl rcc_native_poll\nrcc_native_poll: ret\n')
        reject([*args,reserved],'reserved runtime helper')

        # ELF relocation width/sign boundaries, independent of the assembler's
        # own constant validation. SHN_ABS values carry all 64 bits on the wire.
        scalar=assemble('scalar','.text\n.globl entry\nentry: ret\n.data\n.quad constant\n')
        original=scalar.read_bytes();_,scalar_sections=sections(original)
        record=next(s for s in scalar_sections if s[1]==4 and s[5])[4]
        symbol_bits=struct.unpack_from('<Q',original,record+8)[0]&~0xffffffff
        definition=bytes(data)
        cases=[(1,2**64-1,True),(10,2**32-1,True),(10,2**32,False),
               (11,2**31-1,True),(11,2**64-2**31,True),(11,2**31,False),
               (12,65535,True),(12,65536,False),(14,255,True),(14,256,False)]
        for kind,value,valid in cases:
            b=bytearray(original);struct.pack_into('<Q',b,record+8,symbol_bits|kind);scalar.write_bytes(b)
            b=bytearray(definition);struct.pack_into('<Q',b,sym+8,value);constant_object.write_bytes(b)
            args2=['--metadata',meta,scalar,constant_object]
            if valid:
                subprocess.run([tool,*map(str,args2),'-o',str(out)],check=True)
                image(out.read_bytes())
            else:reject(args2,'absolute width overflow')

        # A table may precede its target section: relocations must be applied
        # AFTER every section's contents have been copied into the image.
        b=bytearray(base);order=list(range(len(ss)));order[1],order[rela]=order[rela],order[1]
        remap={old:new for new,old in enumerate(order)}
        for new,old in enumerate(order):
            row=list(ss[old])
            if row[6]:row[6]=remap[row[6]]
            if row[1]==4:row[7]=remap[row[7]]
            struct.pack_into('<IIQQQQIIQQ',b,shoff+64*new,*row)
        struct.pack_into('<H',b,62,remap[struct.unpack_from('<H',base,62)[0]])
        st=ss[symtab]
        for at in range(st[4],st[4]+st[5],24):
            index=struct.unpack_from('<H',b,at+6)[0]
            if index in remap:struct.pack_into('<H',b,at+6,remap[index])
        broken.write_bytes(b)
        subprocess.run([tool,'--metadata',str(meta),str(broken),str(second),'-o',str(out)],check=True)
        _,_,reordered,functions,_,_=image(out.read_bytes())
        entry=functions[0][2]
        assert reordered[entry]==0xe8 and reordered[entry+5+struct.unpack_from('<i',reordered,entry+1)[0]]==0xc3

        # A PC32 patch can cross an image storage chunk boundary. The native
        # address space and on-disk image must remain completely contiguous.
        boundary=assemble('boundary','.text\n.zero 65534\n.globl entry\nentry: call other\nret\n')
        subprocess.run([tool,'--metadata',str(meta),str(boundary),str(second),'-o',str(out)],check=True)
        _,_,crossed,functions,_,_=image(out.read_bytes())
        entry=functions[0][2]
        assert crossed[entry]==0xe8
        assert crossed[entry+5+struct.unpack_from('<i',crossed,entry+1)[0]]==0xc3

        # Deterministic hostile-input smoke: accepted mutations must still
        # serialize a structurally valid container; no signal/crash is allowed.
        rng=random.Random(171)
        for trial in range(128):
            b=bytearray(base)
            for _ in range(rng.randrange(1,5)):
                b[rng.randrange(len(b))]=rng.randrange(256)
            broken.write_bytes(b);out.write_bytes(b'preserve')
            r=subprocess.run([tool,'--metadata',str(meta),str(broken),str(second),'-o',str(out)],capture_output=True)
            assert r.returncode in (0,1),(trial,r.returncode,r.stderr)
            if r.returncode: assert out.read_bytes()==b'preserve'
            else: image(out.read_bytes())
    print(f'RXE1 layout/imports/BSS/alignment/multi-object relocations, {failures} rejection/atomicity cases and 128 mutations passed')


if __name__=='__main__':main()
