#!/usr/bin/env python3
"""Reproduce the 64-move failure and execute large phi joins independently."""
import ctypes
import os
from pathlib import Path
import platform
import subprocess
import tempfile

from build import ensure_host


def fixture(name, count):
    lines = [f'export function l ${name}(l %condition, l %x) {{', '@entry',
             ' jnz %condition, @left, @right', '@left']
    lines += [f' %a{i} =l add %x, {i + 1}' for i in range(count)]
    lines += [' jmp @join', '@right']
    lines += [f' %b{i} =l mul %x, {i + 1}' for i in range(count)]
    lines += [' jmp @join', '@join']
    lines += [f' %p{i} =l phi @left %a{i}, @right %b{i}' for i in range(count)]
    last = 'p0'
    for i in range(1, count):
        lines.append(f' %s{i} =l add %{last}, %p{i}')
        last = f's{i}'
    return '\n'.join(lines + [f' ret %{last}', '}', ''])


def main():
    original, fixed = ensure_host(), ensure_host(True)
    with tempfile.TemporaryDirectory(prefix='qbe-parallel-moves-') as folder:
        work = Path(folder)
        source, assembly = work / 'many.ssa', work / 'many.s'
        source.write_text(fixture('many', 128))
        old = subprocess.run([str(original), '-t', 'amd64_sysv', '-o', str(assembly), str(source)],
                             capture_output=True, text=True, timeout=30)
        assert old.returncode and 'no more pm slots' in old.stderr, old.stderr
        source.write_text(fixture('many', 128) + fixture('few', 2) + fixture('again', 192))
        target = 'arm64_apple' if platform.system() == 'Darwin' and platform.machine() == 'arm64' else 'amd64_apple' if platform.system() == 'Darwin' else 'amd64_sysv'
        subprocess.run([str(fixed), '-t', target, '-o', str(assembly), str(source)], check=True, timeout=30)
        library = work / 'moves.dylib'
        command = ['cc', '-shared', '-fPIC', str(assembly), '-o', str(library)]
        if os.environ.get('SDKROOT'):
            command += ['-isysroot', os.environ['SDKROOT']]
        subprocess.run(command, check=True)
        module = ctypes.CDLL(str(library))
        for name, count in [('many', 128), ('few', 2), ('again', 192)]:
            function = getattr(module, name)
            function.argtypes = [ctypes.c_uint64, ctypes.c_uint64]
            function.restype = ctypes.c_uint64
            for x in [0, 1, 37, 0xffffffff, 0x8000000000000000]:
                for branch in [0, 1]:
                    expected = sum(x + i if branch else x * i for i in range(1, count + 1))
                    assert function(branch, x) == expected & 0xffffffffffffffff, (name, branch, x)
        small = work / 'small.ssa'
        small.write_text(fixture('small', 2))
        outputs = []
        for tool in [original, fixed]:
            out = work / (tool.parent.name + '.s')
            subprocess.run([str(tool), '-t', target, '-o', str(out), str(small)], check=True)
            outputs.append(out.read_bytes())
        assert outputs[0] == outputs[1], 'storage sizing changed small-function instruction selection'
        print('Original 64-move failure reproduced; large/small/large phi joins execute correctly; small output unchanged')


if __name__ == '__main__':
    main()
