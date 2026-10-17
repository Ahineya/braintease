#!/usr/bin/env python3
"""Check bank-spanning RXE execution, code-pool exhaustion and reuse in FUZIX."""
import argparse
from launch import require_mmu
import json
import struct
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
require_mmu(build)
engines = [True, False] if a.engine == 'both' else [a.engine == 'jit']

# Query the actual link inputs: globbing an incremental build can include stale
# objects for routines now implemented elsewhere in the port.
libs = subprocess.check_output([
    'make', '-s', '--no-print-directory', '-C', str(PORT), f'BUILD={build}',
    '-f', 'Makefile', '-f', '-', 'codebank-libs'
], input='codebank-libs:\n\t@echo $(LIBC_OBJS)\n', text=True).split()

source = r'''
#include <stdio.h>
#include <errno.h>
#include <string.h>
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
static int holder(pid_t *child) {
    int fd[2], status, n;
    char marker;
    char *args[] = {"widecode", "--hold", NULL};
    if (pipe(fd)) return 'X';
    *child = fork();
    if (*child < 0) { close(fd[0]); close(fd[1]); return 'X'; }
    if (!*child) {
        close(fd[0]);
        if (dup2(fd[1], 1) < 0) _exit(99);
        close(fd[1]);
        execv("/bin/widecode", args);
        marker = errno == ENOMEM ? 'E' : 'X';
        write(1, &marker, 1);
        _exit(17);
    }
    close(fd[1]);
    do { n = read(fd[0], &marker, 1); } while (n < 0 && errno == EINTR);
    close(fd[0]);
    if (n == 1 && marker == 'R') return marker;
    if (n != 1) kill(*child, SIGKILL);
    while (waitpid(*child, &status, 0) < 0 && errno == EINTR);
    return n == 1 && WIFEXITED(status) && WEXITSTATUS(status) == 17 ? marker : 'X';
}
static int pressure(void) {
    pid_t children[12], retry;
    int count = 0, marker = 'X', status, i, failed;
    while (count < 12) {
        marker = holder(children + count);
        if (marker != 'R') break;
        count++;
    }
    failed = count != 2 || marker != 'E';
    for (i = 0; i < count; i++) {
        kill(children[i], SIGKILL);
        while (waitpid(children[i], &status, 0) < 0 && errno == EINTR);
    }
    marker = holder(&retry);
    if (marker == 'R') {
        kill(retry, SIGKILL);
        while (waitpid(retry, &status, 0) < 0 && errno == EINTR);
    } else failed = 1;
    if (failed) { printf("codebanks: pressure failed count=%d marker=%c\n", count, marker); return 8; }
    puts("codebanks: allocation pressure, rollback and reuse passed");
    return 0;
}
int main(int argc, char **argv) {
    int status, value;
    pid_t child;
    int (*fn)(int) = large;
    void (*back)(void) = jump_back;
    if (argc > 1 && !strcmp(argv[1], "--hold")) {
        if (write(1, "R", 1) != 1) return 9;
        for (;;) pause();
    }
    if (argc > 1 && !strcmp(argv[1], "--pressure")) return pressure();
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
        '--bank-size', '64000', '-DRIPPLE_MMU=1', '-DNO_64BIT',
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
    # Move the whole image above virtual bank 12, retaining canonical offsets.
    # This tests the former executable limit and signal/pointer validation in
    # high code banks without compiling a megabyte of repetitive source.
    bank_prefix = 13
    prefix = bank_prefix * 64000
    entry, count, data, relocations = struct.unpack_from('<IIII', binary, 8)
    code = bytearray(binary[24:24 + count * 8])
    relocs = bytearray(binary[24 + count * 8 + data:])
    for i in range(relocations):
        kind, reserved, index = struct.unpack_from('<HHI', relocs, i * 8)
        assert not reserved
        if kind == 0:
            bank = struct.unpack_from('<H', code, index * 8 + 4)[0]
            struct.pack_into('<H', code, index * 8 + 4, bank + bank_prefix)
        struct.pack_into('<I', relocs, i * 8 + 4, index + prefix)
    wide = bytearray(binary[:24])
    struct.pack_into('<II', wide, 8, entry + prefix, count + prefix)
    wide += bytes([0x42, 0x42, 0, 0, 0, 0, 0, 0]) * prefix
    wide += code + binary[24 + count * 8:24 + count * 8 + data] + relocs
    (temporary / 'widecode.rxe').write_bytes(wide)
    # Reuse the built OS/apps but keep every disk write in a private directory.
    for file in build.iterdir():
        if file.suffix == '.rxe' or file.name in ('FUZIX.SYS', 'bios.bin'):
            (temporary / file.name).symlink_to(file)
    names = json.loads((build / 'apps.json').read_text())
    (temporary / 'apps.json').write_text(json.dumps([*names, 'codebanks', 'widecode']))
    for jit in engines:
        image = temporary / ('jit.img' if jit else 'interpreter.img')
        subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(temporary),
                        str(image)], check=True)
        commands = ('codebanks\necho BANK_STATUS:$?\nwidecode\necho BANK_STATUS:$?\n' * 2) + 'shutdown\n'
        result = subprocess.run([
            str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'),
            '--disk', str(image), '--memory', '16384000', '--headless',
            '--input', commands, '--timeout', '180', *(['--jit'] if jit else [])
        ], capture_output=True, text=True, timeout=190)
        output = result.stdout + result.stderr
        assert result.returncode == 0, output
        assert output.count('codebanks: far branches, fallthrough, pointers, signals, jumps and fork passed') == 4, output
        assert output.count('\nBANK_STATUS:0\n') == 4, output
        assert 'Ripple: disk synced; power off.' in output, output
        subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)
        # A small advertised host budget must become a recoverable guest ENOMEM,
        # leave existing images intact and return freed extents to the pool.
        result = subprocess.run([
            str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'),
            '--disk', str(image), '--memory', '16384000', '--code-memory', str(80 * 64000),
            '--headless', '--input', 'codebanks --pressure\necho POOL_STATUS:$?\nshutdown\n',
            '--timeout', '180', *(['--jit'] if jit else [])
        ], capture_output=True, text=True, timeout=190)
        output = result.stdout + result.stderr
        assert result.returncode == 0, output
        assert 'codebanks: allocation pressure, rollback and reuse passed' in output, output
        assert '\nPOOL_STATUS:0\n' in output and 'Ripple: disk synced; power off.' in output, output
        subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)
        print(('JIT' if jit else 'Interpreter') + f': bank-spanning RXE, code-pool exhaustion and reuse passed (MMU=1)', flush=True)
