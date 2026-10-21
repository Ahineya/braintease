#!/usr/bin/env python3
"""Independent LLVM encoding/ELF checks for the portable x86 assembler."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def elf(path):
    data = path.read_bytes()
    assert data[:7] == b'\x7fELF\x02\x01\x01'
    assert struct.unpack_from('<HH', data, 16) == (1, 62)
    at = struct.unpack_from('<Q', data, 40)[0]
    size, count, strings = struct.unpack_from('<HHH', data, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, at + size*i) for i in range(count)]
    sh = sections[strings]; names = data[sh[4]:sh[4]+sh[5]]
    result = {}
    for s in sections:
        name = names[s[0]:names.index(0, s[0])].decode()
        assert s[1] == 8 or s[4]+s[5] <= len(data)
        result[name] = (s, data[s[4]:s[4]+s[5]] if s[1] != 8 else b'')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool', type=Path, default=HERE.parent/'build/rcc-x64-as')
    p.add_argument('--qbe', type=Path, default=HERE.parent/'build/qbe-host/qbe')
    a = p.parse_args(); tool = str(a.tool.resolve())
    # Choose canonical forms with one encoding in both assemblers. Other legal
    # choices (mov reg/reg direction, short branches, accumulator immediates)
    # are checked by execution, rather than comparing unrelated encodings.
    instructions = []
    for reg, byte in [('rax','al'),('rdi','dil'),('r8','r8b'),('r15','r15b')]:
        for address in ['(%rsp)','-8(%rbp)','128(%r12,%r13,4)','(,%r9,8)']:
            instructions += [f'movq %{reg}, {address}', f'movb %{byte}, {address}',
                             f'movsbq %{byte}, %{reg}', f'leaq {address}, %{reg}',
                             f'addq {address}, %{reg}', f'imulq {address}, %{reg}']
        instructions += [f'pushq %{reg}',f'popq %{reg}',f'negq %{reg}',f'idivq %{reg}']
    for reg in ['xmm0','xmm7','xmm8','xmm15']:
        for op in ['addss','addsd','mulsd','divss','sqrtss','xorps','xorpd','ucomisd','cvtss2sd']:
            instructions.append(f'{op} 16(%r12,%r13,2), %{reg}')
        instructions += [f'movaps %{reg}, 16(%rsp)', f'movq %r15, %{reg}', f'cvtsi2sd %r15, %{reg}']
    instructions += ['ret','cqto','cltd','cltq','ud2','endbr64','syscall','hlt','cld']
    instructions += ['movq 4+-15312(%rsp), %rax', 'movq 4- -8(%rsp), %rax']
    instructions += ['addq $1000, %r11','movb $255, %r9b','movw $65535, %r9w',
                     'movq $0x123456789abcdef0, %r11','shlq %cl, %r11','shrq $7, %r11',
                     'movq $42, 0','setne %dil','setae %r15b','cmovneq (%r12), %r15']
    with tempfile.TemporaryDirectory(prefix='rcc-x64-test-') as folder:
        work = Path(folder); source=work/'input.s'; actual=work/'actual.o'; reference=work/'llvm.o'
        source.write_text('.text\n'+'\n'.join(instructions)+'\n')
        subprocess.run([tool,str(source),'-o',str(actual)],check=True)
        subprocess.run(['clang','--target=x86_64-unknown-none-elf','-c',str(source),'-o',str(reference)],check=True)
        x=elf(actual)['.text'][1]; y=elf(reference)['.text'][1]
        assert x==y, 'LLVM instruction encoding differs at octet '+str(next(i for i in range(min(len(x),len(y))) if x[i]!=y[i]))
        source.write_text('.text\n.globl entry\nentry:\ncall external\nmovl $7, item(%rip)\njmp .Lend\n.Lend: ret\n.data\nitem: .quad entry+3\n.bss\n.balign 16\n.zero 32\n')
        subprocess.run([tool,str(source),'-o',str(actual)],check=True)
        obj=elf(actual); rel=obj['.rela.text'][1]
        records=[struct.unpack_from('<QQq',rel,i) for i in range(0,len(rel),24)]
        assert [(offset,info&0xffffffff,addend) for offset,info,addend in records]==[(1,4,-4),(7,2,-8)]
        assert obj['.bss'][0][5]==32 and obj['.bss'][0][8]==16
        assert len(obj['.rela.data'][1])==24
        negatives=['movb $256, %al','movw $65536, %ax','movl $4294967296, %eax',
                   'movq $2147483648, (%rsp)','movq %eax, %rax','addsd %rax, %xmm0',
                   'movd %xmm0, %xmm1','cvtsi2sd %ax, %xmm0','movq (%rax,%rsp), %rax',
                   'movq 2147483648(%rax), %rax','jmp *%eax','movq %rip, %rax',
                   '.section .tdata,"awT"','.byte 256','.short 65536','.bogus 1',
                   'x:\nx: ret','/* unfinished','movq $18446744073709551616, %rax',
                   '.balign 3','.zero 16777217','.ascii "\\x100"',
                   '.section .data,"awT"','.section .text,"aw"','.zero 1,256',
                   '.zero 1,0,3','.local missing\n.quad missing',
                   'cvttsd2si %rax, %rax','cvttsd2si_bad %xmm0, %rax',
                   'andss %xmm0, %xmm1','cmovne %eax, %r12','cmovnel %rax, %rax',
                   'movsbgarbageq %al, %rax']
        for text in negatives:
            source.write_text('.text\n'+text+'\n'); actual.write_bytes(b'preserve')
            result=subprocess.run([tool,str(source),'-o',str(actual)],capture_output=True)
            assert result.returncode and result.stderr, 'accepted invalid input: '+text
            assert actual.read_bytes()==b'preserve', 'failed assembly overwrote output'
            assert not list(work.glob('actual.o.tmp.*')), 'temporary output leaked'
        count=0
        for fixture in sorted((HERE.parent/'upstream/qbe/test').glob('[!_]*.ssa')):
            subprocess.run([str(a.qbe.resolve()),'-t','amd64_sysv','-o',str(source),str(fixture)],check=True)
            result=subprocess.run([tool,str(source),'-o',str(actual)],capture_output=True)
            if fixture.stem=='tls':
                assert result.returncode and b'unsupported section' in result.stderr
                continue
            assert not result.returncode, fixture.name+': '+result.stderr.decode()
            elf(actual); count+=1
    print(f'{len(instructions)} LLVM encodings, ELF relocations, {len(negatives)} rejection/atomicity cases and {count} QBE fixtures passed')


if __name__=='__main__': main()
