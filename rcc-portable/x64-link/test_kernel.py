#!/usr/bin/env python3
"""Independently validate ELF kernel segments, sparse BSS and PIE relocations."""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def inspect(data):
    assert data[:7] == b'\x7fELF\x02\x01\x01'
    kind, machine, version, entry, phoff = struct.unpack_from('<HHIQQ', data, 16)
    assert (kind, machine, version, phoff) == (3, 62, 1, 64)
    ehsize, phsize, count = struct.unpack_from('<HHH', data, 52)
    assert (ehsize, phsize, count) == (64, 56, 4)
    rows = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * phsize) for i in range(count)]
    loads = [r for r in rows if r[0] == 1]
    assert [r[1] for r in loads] == [5, 4, 6]
    previous = 0
    for _, flags, offset, address, physical, filesz, memsz, alignment in loads:
        assert address >= previous and physical == address and filesz <= memsz
        assert offset + filesz <= len(data) and offset % alignment == address % alignment
        assert alignment == 4096 and not flags & 1 or flags == 5
        previous = address + memsz
    assert loads[0][3] <= entry < loads[0][3] + loads[0][5]
    memory = bytearray(previous)
    for _, _, offset, address, _, filesz, _, _ in loads:
        memory[address:address + filesz] = data[offset:offset + filesz]
    dynamic = next(r for r in rows if r[0] == 2)
    assert dynamic[5:7] == (64, 64)
    values = dict(struct.unpack_from('<QQ', memory, dynamic[3] + i * 16) for i in range(4))
    assert set(values) == {0, 7, 8, 9} and values[9] == 24 and values[8] % 24 == 0
    relocations = [struct.unpack_from('<QQQ', memory, i) for i in range(values[7], values[7] + values[8], 24)]
    assert all(t == 8 and at + 8 <= previous and add < previous for at, t, add in relocations)
    assert len({at for at, _, _ in relocations}) == len(relocations)
    for base in [0x100000, 0x19a00000, 0x100000000]:
        relocated = bytearray(memory)
        for at, _, add in relocations:
            struct.pack_into('<Q', relocated, at, base + add)
        assert all(struct.unpack_from('<Q', relocated, at)[0] == base + add for at, _, add in relocations)
    return loads, relocations, memory, entry


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--linker', type=Path, default=HERE.parent / 'build/rcc-x64-ld')
    a = p.parse_args()
    linker = str(a.linker.resolve())
    with tempfile.TemporaryDirectory(prefix='rcc-kernel-elf-') as folder:
        work = Path(folder)
        def assemble(name, text):
            source, obj = work / (name + '.s'), work / (name + '.o')
            source.write_text(text)
            subprocess.run(['clang', '--target=x86_64-unknown-none', '-c', str(source), '-o', str(obj)], check=True)
            return str(obj)
        start = assemble('start', '''.text
.globl _start
_start:
    call rcc_native_entry
    ret
''')
        code = assemble('code', '''.text
.globl rcc_native_entry
rcc_native_entry:
    movq data@GOTPCREL(%rip), %rax
    cmpq kernel_end@GOTPCREL(%rip), %rax
    leaq kernel_readonly_end(%rip), %rdx
    ret
.data
.globl data
data:
    .byte 7
    .quad rcc_native_entry
    .quad kernel_end
.bss
.balign 4096
    .zero 33554432
''')
        output = work / 'kernel.elf'
        subprocess.run([linker, '--kernel-elf', start, code, '-o', str(output)], check=True)
        data = output.read_bytes()
        loads, relocs, memory, entry = inspect(data)
        assert len(data) < 65536 and loads[-1][6] > 33554432
        assert len(relocs) == 4 and any(at % 8 for at, _, _ in relocs)
        # The instruction opcodes remain MOV and CMP; GOTPCRELX does not
        # promise that an arbitrary memory operation can turn into LEA.
        target = entry + 5 + struct.unpack_from('<i', memory, entry + 1)[0]
        assert memory[target:target + 3] == b'\x48\x8b\x05'
        got = target + 7 + struct.unpack_from('<i', memory, target + 3)[0]
        assert any(at == got for at, _, _ in relocs)
        assert memory[target + 7:target + 10] == b'\x48\x3b\x05'
        # Repeat byte identity guards layout dependence on host pointers.
        subprocess.run([linker, '--kernel-elf', start, code, '-o', str(work / 'again')], check=True)
        assert (work / 'again').read_bytes() == data
        if shutil.which('llvm-readelf'):
            subprocess.run(['llvm-readelf', '-l', str(output)], check=True, stdout=subprocess.DEVNULL)
        negatives = {
            'missing-start': [code],
            'missing-entry': [start],
            'duplicate': [start, start, code],
            'reserved-layout': [start, code, assemble('reserved', '.data\n.globl kernel_end\nkernel_end: .quad 0\n')],
            'wx': [start, code, assemble('wx', '.section .bad,"awx",@progbits\n.byte 1\n')],
            'undefined': [start, code, assemble('undefined', '.text\ncall missing\n')],
            'narrow-address': [start, code, assemble('narrow', '.data\n.long rcc_native_entry\n')],
        }
        for name, objects in negatives.items():
            output.write_bytes(b'preserved')
            result = subprocess.run([linker, '--kernel-elf', *objects, '-o', str(output)], capture_output=True)
            assert result.returncode == 1 and result.stderr, (name, result)
            assert output.read_bytes() == b'preserved' and not list(work.glob('kernel.elf.tmp.*')), name
        print(f'Kernel ELF segment permissions, 32 MiB sparse BSS, unaligned/GOT relocations, 3 load bases and {len(negatives)} rejections passed')


if __name__ == '__main__':
    main()
