#!/usr/bin/env python3
"""Load a bank-spanning C function through the real FUZIX RXE loader."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
p.add_argument('--engine', choices=('jit', 'interpreter', 'both'), default='jit')
a = p.parse_args()
build = a.build.resolve()
engines = [True, False] if a.engine == 'both' else [a.engine == 'jit']
mmu = (build / '.mmu').read_text().strip()

# Query the actual link inputs: globbing an incremental build can include stale
# objects for routines now implemented elsewhere in the port.
libs = subprocess.check_output([
    'make', '-s', '--no-print-directory', '-C', str(PORT), f'BUILD={build}',
    '-f', 'Makefile', '-f', '-', 'codebank-libs'
], input='codebank-libs:\n\t@echo $(LIBC_OBJS)\n', text=True).split()

source = r'''
#include <stdio.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/wait.h>
static volatile int caught;
static jmp_buf jump;
PADDING
int large(int mode) {
    int i, total = 0;
    if (!mode) return 7;
    for (i = 0; i < 2; i++) {
        __asm__(N65536);
        total += mode;
    }
    return total + 40;
}
static void catch_signal(int sig) { caught = sig; }
static void jump_back(void) { longjmp(jump, 23); }
int main(void) {
    int status, value;
    pid_t child;
    int (*fn)(int) = large;
    void (*back)(void) = jump_back;
    if (fn(0) != 7 || fn(1) != 42) return 1;
    if (signal(SIGUSR1, catch_signal) == SIG_ERR ||
        kill(getpid(), SIGUSR1) || caught != SIGUSR1) return 2;
    value = setjmp(jump);
    if (!value) back();
    if (value != 23) return 3;
    child = fork();
    if (child < 0) return 4;
    if (!child) _exit(fn(2) == 44 ? 17 : 5);
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 17) return 6;
    puts("codebanks: far branches, fallthrough, pointers, signals, jumps and fork passed");
    return 0;
}
'''
padding = '#define N1 "NOP\\n"\n'
for power in range(1, 17):
    previous, current = 1 << (power - 1), 1 << power
    padding += f'#define N{current} N{previous} N{previous}\n'

with tempfile.TemporaryDirectory(prefix='fuzix-codebanks-') as directory:
    temporary = Path(directory)
    c, asm, obj, exe = [temporary / ('codebanks' + suffix)
                        for suffix in ('.c', '.asm', '.pobj', '.rxe')]
    c.write_text(source.replace('PADDING', padding))
    subprocess.run([
        str(ROOT / 'target/release/rcc'), 'compile', str(c), '-o', str(asm),
        '--bank-size', '64000', f'-DRIPPLE_MMU={mmu}', '-DNO_64BIT',
        *[f'-I{path}' for path in (PORT / 'ripple/include',
                                 PORT / 'upstream/Library/include',
                                 ROOT / 'runtime/include', PORT / 'ripple')]
    ], check=True)
    subprocess.run([str(ROOT / 'src/ripple-asm/target/release/rasm'),
                    'assemble', str(asm), '-o', str(obj), '--bank-size',
                    '64000', '--max-immediate', '65535'], check=True)
    compiled = json.loads(obj.read_text())
    assert len(compiled['instructions']) > 65536
    subprocess.run([str(ROOT / 'src/ripple-asm/target/release/rlink'),
                    str(build / 'user/entry.pobj'), str(obj),
                    str(build / 'user/userlib.pobj'), *libs,
                    '-f', 'rxe', '--bank-size', '64000', '-o', str(exe)], check=True)
    binary = exe.read_bytes()
    assert binary[:4] == b'RXE1'
    assert int.from_bytes(binary[12:16], 'little') > 65536
    # Reuse the built OS/apps but keep every disk write in a private directory.
    for file in build.iterdir():
        if file.suffix == '.rxe' or file.name in ('FUZIX.SYS', 'bios.bin'):
            (temporary / file.name).symlink_to(file)
    names = json.loads((build / 'apps.json').read_text())
    (temporary / 'apps.json').write_text(json.dumps([*names, 'codebanks']))
    for jit in engines:
        image = temporary / ('jit.img' if jit else 'interpreter.img')
        subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(temporary),
                        str(image)], check=True)
        commands = ('codebanks\necho BANK_STATUS:$?\n' * 2) + 'shutdown\n'
        result = subprocess.run([
            str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'),
            '--disk', str(image), '--memory', '16384000', '--headless',
            '--input', commands, '--timeout', '180', *(['--jit'] if jit else [])
        ], capture_output=True, text=True, timeout=190)
        output = result.stdout + result.stderr
        assert result.returncode == 0, output
        assert output.count('codebanks: far branches, fallthrough, pointers, signals, jumps and fork passed') == 2, output
        assert output.count('\nBANK_STATUS:0\n') == 2, output
        assert 'Ripple: disk synced; power off.' in output, output
        subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)
        print(('JIT' if jit else 'Interpreter') + f': bank-spanning RXE passed (MMU={mmu})', flush=True)
