#!/usr/bin/env python3
"""Check the Rust cell ABI, diagnostics, and both disposable guest executions."""
import argparse
import errno
import json
import os
from pathlib import Path
import pty
import select
import subprocess
import sys
import tempfile
import time

import compile as compiler

ROOT, HERE = compiler.ROOT, compiler.HERE


class Terminal:
    def __init__(self, command, environment):
        self.master, slave = pty.openpty()
        self.child = subprocess.Popen(list(map(str, command)), stdin=slave, stdout=slave, stderr=slave,
                                      cwd=ROOT, env=environment, start_new_session=True)
        os.close(slave)
        self.buffer = bytearray()
        self.log = bytearray()

    def until(self, marker, timeout=90, unexpected=None):
        deadline = time.monotonic() + timeout
        while marker not in self.buffer:
            if unexpected and unexpected in self.buffer:
                raise AssertionError('guest returned before ' + repr(marker) + '\n' + self.log[-8000:].decode(errors='replace'))
            if time.monotonic() >= deadline:
                raise AssertionError(f'timeout waiting for {marker!r}\n' + self.log[-8000:].decode(errors='replace'))
            if select.select([self.master], [], [], .1)[0]:
                try:
                    data = os.read(self.master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        raise AssertionError(self.log[-8000:].decode(errors='replace')) from error
                    raise
                if not data:
                    raise AssertionError(self.log[-8000:].decode(errors='replace'))
                self.buffer.extend(data.replace(b'\r', b''))
                self.log.extend(data)
        end = self.buffer.index(marker) + len(marker)
        before = bytes(self.buffer[:end])
        del self.buffer[:end]
        return before

    def send(self, value):
        os.write(self.master, value)

    def close(self):
        if self.child.poll() is None:
            self.child.kill()
        self.child.wait(timeout=10)
        os.close(self.master)


def diagnostics(driver, sysroot, directory):
    cases = {
        'borrow': ('let mut value=1u16; let r=&mut value; let other=&mut value; *r += *other;',
                   'cannot borrow'),
        'moved_drop': ('struct Resource; impl Drop for Resource { fn drop(&mut self) {} } '
                       'let value=Resource; core::mem::drop(value); core::mem::drop(value);',
                       'use of moved value'),
        'wide': ('let value=1u128; unsafe { core::ptr::read_volatile(&value); }',
                   'unsupported constant: u128'),
        'ffi': ('unsafe extern "C" { fn unbridged(); } unsafe { unbridged(); }',
                'unapproved C16 FFI symbol'),
        'allocator_ffi': ('unsafe extern "C" { fn rustcell_alloc(size:usize, align:usize) -> *mut u8; } '
                          'unsafe { rustcell_alloc(4, 4); }', 'unapproved C16 FFI symbol'),
        'custom_allocator': ('struct Heap; unsafe impl core::alloc::GlobalAlloc for Heap { '
                             'unsafe fn alloc(&self, _:core::alloc::Layout) -> *mut u8 { core::ptr::null_mut() } '
                             'unsafe fn dealloc(&self, _: *mut u8, _:core::alloc::Layout) {} } '
                             '#[global_allocator] static HEAP:Heap=Heap;', 'custom global allocators are not supported'),
        'mutable_static': ('static mut VALUE:u32=7; '
                           'unsafe { rustcell_report(core::ptr::read(&raw const VALUE) as u16); }',
                           'mutable constant storage is not supported'),
        'function_static': ('extern "C" fn handler()->u16 { 7 } static TABLE:[extern "C" fn()->u16;1]=[handler]; '
                             'unsafe { core::ptr::read_volatile(&TABLE); }',
                             'only Rust function-pointer ABIs are supported'),
        'foreign_function_pointer': ('unsafe extern "Rust" { fn unbridged()->u16; } '
                                      'let callback:unsafe fn()->u16=unbridged; unsafe { rustcell_report(callback()); }',
                                      'foreign function pointers require an explicit bridge'),
        'dyn_tail': ('trait Value { fn value(&self)->u16; } impl Value for u16 { fn value(&self)->u16 { *self } } '
                      'struct Tail<T:?Sized> { head:u8, data:T } let data=Tail { head:1, data:7u16 }; '
                      'let view:&Tail<dyn Value>=&data; unsafe { rustcell_report(view.data.value()); }',
                      'struct tails containing trait objects are not supported'),
        'aligned_static': ('#[repr(align(1024))] struct Aligned(u8); static VALUE:Aligned=Aligned(7); '
                            'unsafe { rustcell_report(core::ptr::read(&VALUE.0) as u16); }',
                            'constant storage exceeds prototype size/alignment limits'),
        'signature': ('unsafe extern "C" { fn rustcell_write(value:u16); } unsafe { rustcell_write(1); }',
                      'incorrect bridge signature'),
        'file_signature': ('unsafe extern "C" { fn rustcell_close(fd:i32) -> isize; } '
                           'unsafe { rustcell_close(1); }', 'incorrect bridge signature'),
        'variadic': ('unsafe extern "C" { fn rustcell_report(value:u16,...); } unsafe { rustcell_report(1); }',
                     'incorrect bridge ABI'),
        'link_name': ('unsafe extern "C" { #[link_name="other"] fn rustcell_report(value:u16); } unsafe { rustcell_report(1); }',
                      'incorrect bridge ABI'),
    }
    for name, (body, expected) in cases.items():
        source = directory / (name + '.rs')
        output = directory / (name + '.ir')
        source.write_text('''#![no_std]
#![no_main]
unsafe extern "C" { fn rustcell_panic() -> !; fn rustcell_report(value:u16); }
#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! { unsafe { rustcell_panic() } }
pub fn rust_entry() -> u16 { ''' + body + ' 0 }\n')
        result = subprocess.run([str(driver), str(source), '--edition', '2024', '--crate-type', 'lib',
                    '--target', str(HERE / 'targets/ripple-fuzix.json'), '--sysroot', str(sysroot),
                    '-Zunstable-options', '-Cpanic=abort', '-Copt-level=1', '--emit', 'metadata',
                    '--rcc-output', str(output)], capture_output=True, text=True)
        assert result.returncode != 0 and expected in result.stderr, result.stderr
        assert 'internal compiler error' not in result.stderr, result.stderr
        assert not output.exists(), 'failed compile emitted IR'
    print('Rust diagnostics: borrow checking and unsupported ABI/features rejected correctly', flush=True)


def host_probe(driver, sysroot, directory, rcc):
    ir = directory / 'probe.ir'
    compiler.emit_ir(HERE / 'examples/probe.rs', ir, True, driver, sysroot)
    shim = directory / 'host.ir'
    with shim.open('w') as file:
        compiler.run(rcc, 'compile', HERE / 'tests/host.c', '--target', 'native', '-I', ROOT / 'runtime/include', '--emit-ir', stdout=file)
    program = directory / 'host-probe'
    compiler.run(rcc, 'link-native', shim, ir, '-o', program)
    result = subprocess.run([str(program)])
    assert result.returncode == 42, f'host cell representation probe: {result.returncode}'
    print('Native cell layout: size_of(u16)=2, bytes=34/12, high-byte write yields 0x5634', flush=True)


def values_probe(driver, sysroot, directory, rcc, selected):
    reference = directory / 'values-reference'
    compiler.run('rustup', 'run', selected, 'rustc', HERE / 'tests/values-reference.rs',
                 '--edition', '2024', '-Copt-level=1', '-Coverflow-checks=off', '-Cpanic=abort', '-o', reference)
    compiler.run(reference)
    ir = directory / 'values.ir'
    compiler.emit_ir(HERE / 'tests/values-entry.rs', ir, True, driver, sysroot)
    shim = directory / 'values-host.ir'
    with shim.open('w') as file:
        compiler.run(rcc, 'compile', HERE / 'tests/values-host.c', '--target', 'native', '-I', ROOT / 'runtime/include', '--emit-ir', stdout=file)
    program = directory / 'values-probe'
    compiler.run(rcc, 'link-native', shim, ir, '-o', program)
    compiler.run(program)
    print('RCC native: enum and destructor checks match the Rust host reference', flush=True)


def heap_probe(driver, sysroot, directory, rcc, selected):
    reference = directory / 'heap-reference'
    compiler.run('rustup', 'run', selected, 'rustc', HERE / 'tests/heap-reference.rs',
                 '--edition', '2024', '-Copt-level=1', '-Coverflow-checks=off', '-Cpanic=abort', '-o', reference)
    compiler.run(reference)
    ir = directory / 'heap.ir'
    compiler.emit_ir(HERE / 'tests/heap-entry.rs', ir, True, driver, sysroot)
    shim = directory / 'heap-host.ir'
    with shim.open('w') as file:
        compiler.run(rcc, 'compile', HERE / 'tests/heap-host.c', '--target', 'native',
                     '-I', ROOT / 'runtime/include', '--emit-ir', stdout=file)
    program = directory / 'heap-probe'
    compiler.run(rcc, 'link-native', shim, ir, '-o', program)
    subprocess.run([str(program)], check=True, timeout=90)
    print('RCC native: Box/Vec/String, aligned cross-bank allocation, failure recovery and drop checks passed', flush=True)


def guest(system, app, upper, build, executable, output):
    with tempfile.TemporaryDirectory(prefix='rustcell-' + system + '-') as temporary:
        directory = Path(temporary)
        image = directory / 'test.img'
        compiler.run('node', HERE / 'tests/image.mjs', system, build, app, image, upper)
        check_image(system, image)
        environment = dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off',
                           RCC_NATIVE_TIMEOUT='180', RIPPLE_NET='off')
        if system == 'fuzix':
            command = [ROOT / 'target/release/rvm', build / 'bios.bin', '--disk', image,
                       '--memory', '16384000', '--jit', '--net', 'off', '--audio', 'off', '--timeout', '180']
            prompt = b'riplix# '
        else:
            command = [executable, '--run', image]
            prompt = b'fuse-x# '
        terminal = Terminal(command, environment)
        try:
            terminal.until(prompt)
            print(f'{system}: guest booted; running Rust probe', flush=True)
            # Queue cleanup in the shell's parsed command list. Sending it at a
            # later prompt races terminal input/prompt transitions, and the
            # newline left by the probe's one-byte read can add another prompt.
            # Check $? immediately after the probe, before any other command.
            terminal.send(b"rustprobe /input /output 'two words' ''; echo RUSTCELL_STATUS:$?; "
                          b"upper /input /second; echo UPPER:$?; "
                          b"upper /missing /bad; echo MISSING:$?; upper; echo USAGE:$?; shutdown\r")
            terminal.until(b'RUSTCELL_WAITING', unexpected=prompt)
            print(f'{system}: layout checks passed; testing delayed input', flush=True)
            # Drain output while withholding input. This proves the application
            # remains blocked, rather than merely accepting prequeued input.
            try:
                terminal.until(b'RUSTCELL_IO_OK', timeout=.25)
            except AssertionError as error:
                if 'timeout waiting' not in str(error):
                    raise
            else:
                raise AssertionError('read returned before input was supplied')
            terminal.send(b'R\r')
            terminal.until(b'RUSTCELL_FORK_OK', unexpected=prompt)
            terminal.until(b'RUSTCELL_STATUS:0\n')
            terminal.until(b'UPPER:0\n')
            terminal.until(b'MISSING:1\n')
            terminal.until(b'USAGE:2\n')
            if system == 'fuzix':
                terminal.until(b'Halted.', timeout=30)
            else:
                terminal.until(b'filesystem synced; session stopped', timeout=30)
            assert terminal.child.wait(timeout=30) == 0
            text = terminal.log.decode(errors='replace').replace('\r', '')
            for marker in ['LAYOUT', 'ENUMS', 'DROPS', 'HEAP', 'FILES', 'IO', 'CHILD', 'FORK']:
                assert f'RUSTCELL_{marker}_OK\n' in text, text
            assert 'RUSTCELL_PANIC' not in text, text
            assert text.count('RUSTCELL_VALUE 901\n') == 1, 'zero-sized destructor did not run exactly once\n' + text
        finally:
            (output / (system + '.log')).write_bytes(terminal.log)
            terminal.close()
        check_image(system, image)
        compiler.run('node', HERE / 'tests/check-files.mjs', system, image)
    print(f'{system}: Rust layouts, collections, files/args/utility, destructors, blocking read, heap fork/wait and shutdown passed', flush=True)


def check_image(system, image):
    if system == 'fuzix':
        compiler.run('python3', ROOT / 'ports/fuzix/tools/fsck.py', image)
    else:
        compiler.run('node', '--input-type=module', '-e', '''
import {checkRoot} from './ports/fuzix/tools/fsck.mjs';
import {openImage} from './ports/fuzix/tools/image.mjs';
const result = checkRoot(openImage(process.argv[1]));
if (result.issues.length) throw Error(result.issues.join('; '));
console.log(`Fuse-X root: ${result.files} inodes; clean`);
''', image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--system', choices=['both', 'fuzix', 'fuse-x', 'host'], default='both')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/rust-rcc')
    parser.add_argument('--fuzix-build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--fuse-x-build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    parser.add_argument('--toolchain')
    parser.add_argument('--skip-build', action='store_true', help='reuse previously compiled guest probes')
    args = parser.parse_args()
    output = args.build.resolve()
    output.mkdir(parents=True, exist_ok=True)
    selected = compiler.toolchain(args.toolchain)
    driver, sysroot = compiler.prepare(output, selected)
    with tempfile.TemporaryDirectory(prefix='rustcell-checks-') as temporary:
        directory = Path(temporary)
        diagnostics(driver, sysroot, directory)
        host_probe(driver, sysroot, directory, args.compiler.resolve())
        values_probe(driver, sysroot, directory, args.compiler.resolve(), selected)
        heap_probe(driver, sysroot, directory, args.compiler.resolve(), selected)
    systems = ['fuse-x', 'fuzix'] if args.system == 'both' else ([] if args.system == 'host' else [args.system])
    for system in systems:
        app = output / ('guest.fxe' if system == 'fuse-x' else 'guest.rxe')
        upper = output / ('upper.fxe' if system == 'fuse-x' else 'upper.rxe')
        if not args.skip_build:
            for source, executable in [('guest.rs', app), ('upper.rs', upper)]:
                compiler.run('python3', HERE / 'compile.py', HERE / 'examples' / source, '--target', system,
                             '-o', executable, '--build', output, '--compiler', args.compiler.resolve(),
                             '--fuzix-build', args.fuzix_build.resolve(), '--fuse-x-build', args.fuse_x_build.resolve(),
                             '--toolchain', selected)
        guest(system, app, upper, args.fuzix_build.resolve() if system == 'fuzix' else args.fuse_x_build.resolve(),
              args.fuse_x_build.resolve() / 'fuse-x', output)
    report = dict(compiler_commit=compiler.COMMIT, rust_bytes='one cell per octet',
                  diagnostics='passed', native_memory='passed', enum_drop_reference='passed',
                  heap_reference='passed', files_args=systems, guests=systems)
    report_name = 'results.json' if args.system == 'both' else f'results-{args.system}.json'
    (output / report_name).write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
