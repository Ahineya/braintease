#!/usr/bin/env python3
"""Independent PE32+ loader, embedded-payload and base-relocation checks."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--linker', type=Path, default=HERE.parent / 'build/rcc-x64-ld')
    a = p.parse_args()
    tool = str(a.linker.resolve())
    with tempfile.TemporaryDirectory(prefix='rcc-efi-') as folder:
        work = Path(folder)
        source, obj, payload, out = [work / n for n in ['loader.s', 'loader.obj', 'kernel.elf', 'BOOTX64.EFI']]
        source.write_text('''.text
.globl efi_main
efi_main:
 leaq kernel_payload_start(%rip), %rax
 leaq kernel_payload_end(%rip), %rdx
 ret
.data
.globl anchor
anchor: .quad efi_main
.bss
.zero 64
''')
        subprocess.run(['clang', '--target=x86_64-pc-windows-msvc', '-c', str(source), '-o', str(obj)], check=True)
        kernel = bytearray(8193)
        kernel[:7] = b'\x7fELF\x02\x01\x01'
        struct.pack_into('<HH', kernel, 16, 3, 62)
        kernel[64:] = bytes((i % 256 for i in range(len(kernel) - 64)))
        payload.write_bytes(kernel)
        command = [tool, '--efi-loader', str(obj), '--kernel-payload', str(payload), '-o', str(out)]
        subprocess.run(command, check=True)
        data = out.read_bytes()
        assert data[:2] == b'MZ'
        pe = struct.unpack_from('<I', data, 60)[0]
        assert data[pe:pe + 4] == b'PE\0\0'
        machine, count = struct.unpack_from('<HH', data, pe + 4)
        assert machine == 0x8664 and count == 5
        optional = pe + 24
        assert struct.unpack_from('<H', data, optional)[0] == 0x20b
        entry = struct.unpack_from('<I', data, optional + 16)[0]
        base = struct.unpack_from('<Q', data, optional + 24)[0]
        size = struct.unpack_from('<I', data, optional + 56)[0]
        headers = struct.unpack_from('<I', data, optional + 60)[0]
        assert struct.unpack_from('<H', data, optional + 68)[0] == 10
        rows = []
        memory = bytearray(size)
        for i in range(count):
            r = optional + 240 + i * 40
            name, virtual, rva, raw, at = struct.unpack_from('<8sIIII', data, r)
            flags = struct.unpack_from('<I', data, r + 36)[0]
            assert rva % 4096 == 0 and at % 512 == 0 and raw % 512 == 0
            assert at + raw <= len(data) and rva + virtual <= size
            assert not (flags & 0x20000000 and flags & 0x80000000)
            if raw:
                assert at >= headers
                memory[rva:rva + virtual] = data[at:at + virtual]
            else:
                assert flags & 0x80 and at == 0
            rows.append((name.rstrip(b'\0'), virtual, rva, flags))
        embedded = next(r for r in rows if r[0] == b'.kernel')
        assert memory[embedded[2]:embedded[2] + embedded[1]] == kernel
        # Both RIP-relative symbols refer to the actual copied ELF extent.
        assert memory[entry:entry + 3] == b'\x48\x8d\x05'
        assert entry + 7 + struct.unpack_from('<i', memory, entry + 3)[0] == embedded[2]
        assert entry + 14 + struct.unpack_from('<i', memory, entry + 10)[0] == embedded[2] + len(kernel)
        reloc, length = struct.unpack_from('<II', data, optional + 112 + 5 * 8)
        assert length == 12
        page, length, fix, pad = struct.unpack_from('<IIHH', memory, reloc)
        assert length == 12 and fix >> 12 == 10 and pad == 0
        fix_at = page + (fix & 4095)
        assert struct.unpack_from('<Q', memory, fix_at)[0] == base + entry
        for destination in [0x200000, 0x1f300000, 0x100000000]:
            relocated = bytearray(memory)
            struct.pack_into('<Q', relocated, fix_at, struct.unpack_from('<Q', relocated, fix_at)[0] + destination - base)
            assert struct.unpack_from('<Q', relocated, fix_at)[0] == destination + entry
        subprocess.run(command, check=True)
        assert out.read_bytes() == data
        disk = work / 'boot.img'
        subprocess.run([tool, '--boot-media', str(out), '-o', str(disk)], check=True)
        media = disk.read_bytes()
        assert len(media) == 16 * 1024 * 1024 and media[510:512] == b'\x55\xaa'
        sector, clusters, reserved, fats, roots, total, marker, fat_size = struct.unpack_from('<HBHBHHBH', media, 11)
        assert (sector, clusters, reserved, fats, roots, total, marker, fat_size) == (512, 1, 1, 2, 512, 32768, 248, 128)
        assert media[512:512 + 65536] == media[129 * 512:257 * 512]
        assert media[257 * 512:257 * 512 + 11] == b'EFI        '
        assert media[289 * 512 + 64:289 * 512 + 75] == b'BOOT       '
        directory = 290 * 512 + 64
        assert media[directory:directory + 11] == b'BOOTX64 EFI'
        first = struct.unpack_from('<H', media, directory + 26)[0]
        length = struct.unpack_from('<I', media, directory + 28)[0]
        recovered = bytearray()
        cluster = first
        visited = set()
        while cluster < 0xfff8:
            assert cluster not in visited and 2 <= cluster < 32481
            visited.add(cluster)
            start = (289 + cluster - 2) * 512
            recovered.extend(media[start:start + 512])
            cluster = struct.unpack_from('<H', media, 512 + cluster * 2)[0]
        assert recovered[:length] == data and length == len(data)
        original = obj.read_bytes()
        negatives = [(0, b'NO'), (2, struct.pack('<H', 65535)), (8, struct.pack('<I', 0xffffffff)),
                     (12, struct.pack('<I', 0xffffffff)), (16, b'\x01'), (20 + 36, struct.pack('<I', 0xe0000020))]
        failures = 0
        for at, change in negatives:
            changed = bytearray(original)
            changed[at:at + len(change)] = change
            obj.write_bytes(changed)
            out.write_bytes(b'preserved')
            result = subprocess.run(command, capture_output=True)
            assert result.returncode == 1 and result.stderr, (at, result)
            assert out.read_bytes() == b'preserved' and not list(work.glob('BOOTX64.EFI.tmp.*'))
            failures += 1
        obj.write_bytes(original)
        payload.write_bytes(b'not an ELF kernel')
        out.write_bytes(b'preserved')
        result = subprocess.run(command, capture_output=True)
        assert result.returncode == 1 and out.read_bytes() == b'preserved'
        print(f'EFI bounds, permissions, RIP symbols, 3 relocation bases, FAT16 recovery and {failures + 1} atomic rejections passed')


if __name__ == '__main__':
    main()
