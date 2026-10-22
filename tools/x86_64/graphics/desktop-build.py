#!/usr/bin/env python3
"""Build an isolated native desktop EFI; never installs or opens a root disk.

The HP profile reuses a hash-pinned, private accepted platform build for its
device firmware/identity objects. The fixture profile includes no Wi-Fi or
personal root inputs. Both use freshly emitted C16 kernel code and shared policy.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools/x86_64'))
from boot import elf_linker, linker, elf_symbols

def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', required=True, type=Path)
    p.add_argument('--kernel-ir', required=True, type=Path)
    p.add_argument('--emitter', type=Path, default=ROOT/'target/release/rcc-native-object')
    p.add_argument('--platform', choices=('fixture', 'hp'), default='fixture')
    p.add_argument('--base', required=True, type=Path, help='retained platform objects/font atlas')
    p.add_argument('--base-efi-sha256', required=True, help='independently accepted platform EFI hash')
    p.add_argument('--kernel-object', type=Path, help='reuse a separately emitted fresh kernel object')
    a = p.parse_args()
    os.umask(0o077)
    d, base, ir = a.build.resolve(), a.base.resolve(), a.kernel_ir.resolve()
    if d == base or base in d.parents:
        raise RuntimeError('output must be separate from retained platform inputs')
    accepted = base/'esp/EFI/BOOT/BOOTX64.EFI'
    if sha(accepted) != a.base_efi_sha256:
        raise RuntimeError('retained platform EFI hash mismatch')
    d.mkdir(parents=True, exist_ok=True)
    profile = d/'desktop-profile.json'
    identity = dict(platform=a.platform, base=str(base), base_efi_sha256=a.base_efi_sha256)
    if profile.exists() and json.loads(profile.read_text()) != identity:
        raise RuntimeError('refusing to mix different native platform profiles in one output directory')
    profile.write_text(json.dumps(identity, indent=2)+'\n')
    dependencies = {}
    for tree in ('tools/x86_64/boot', 'tools/x86_64/graphics', 'tools/x86_64/math', 'libraries/graphics'):
        for source in sorted((ROOT/tree).rglob('*')):
            if source.is_file() and source.suffix in ('.rs', '.h', '.inc', '.bin', '.toml', '.lock', '.c', '.S', '.ld'):
                dependencies[str(source.relative_to(ROOT))] = sha(source)
    inputs = {}
    if a.platform == 'hp':
        for source in sorted(base.iterdir()):
            if source.is_file() and source.suffix in ('.o', '.h', '.atlas'):
                inputs[source.name] = sha(source)
                shutil.copyfile(source, d/source.name)
    else:
        for name in ('font.o', 'vectors.o'):
            inputs[name] = sha(base/name)
            shutil.copyfile(base/name, d/name)
    guest = d/'rcc.o'
    kernel_inputs = (ir/'freestanding-kernel-inputs.txt').read_text().splitlines()
    app_inputs = (ir/'freestanding-app-inputs.txt').read_text().splitlines()
    if a.kernel_object:
        shutil.copyfile(a.kernel_object.resolve(), guest)
    else:
        args = [part for name in kernel_inputs for part in ('--kernel', name)]
        subprocess.run([str(a.emitter.resolve()), '--triple', 'x86_64-unknown-none',
                        *args, *app_inputs, '-o', str(guest)], check=True)
    defs = ['RIPLIX_BOOT', 'PERSISTENT_ROOT', 'BOOT_MATH', 'BOOT_FONT_ATLAS',
            'BOOT_GRAPHICS', 'BOOT_DESKTOP', 'BOOT_NETWORK']
    sources = ['kernel', 'console', 'input', 'keyboard', 'runtime', 'gx']
    if a.platform == 'hp':
        defs += ['BOOT_WIFI_LIVE', 'BOOT_WIFI_USERLAND', 'BOOT_WIFI_BRINGUP',
                 'BOOT_WIFI_PNVM', 'BOOT_WIFI_SCAN', 'BOOT_WIFI_PREPARE',
                 'BOOT_WIFI_AUTH', 'BOOT_WIFI_ASSOC', 'BOOT_WIFI_WPA2',
                 'BOOT_WIFI_DATA', 'BOOT_ROOT_PARTITION', 'ROOT_NVME', 'BOOT_TOUCHPAD']
        sources += ['i2c-designware', 'hid_i2c', 'i2c-hp', 'touchpad',
                    'pci-resource', 'pci-assign', 'pci-refine']
    else:
        defs += ['BOOT_TEST']
        sources += ['arch', 'timer', 'entropy', 'acpi', 'power', 'pci', 'dma', 'net', 'block']
    common = ['-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-mno-red-zone',
              '-mgeneral-regs-only', '-O2', '-Wall', '-Wextra', '-Werror',
              '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
              '-ffunction-sections', '-fdata-sections', *[f'-D{x}=1' for x in defs],
              '-DCONSOLE_SCALE=0', '-DBOOT_FAULT=0', '-I', str(d)]
    records = {}
    for name in sources:
        source = ROOT/'tools/x86_64/boot'/(name+('.S' if name == 'arch' else '.c'))
        records[str(source.relative_to(ROOT))] = sha(source)
        subprocess.run(['clang', '--target=x86_64-unknown-none-elf', *common, '-fpie',
                        '-c', str(source), '-o', str(d/(name+'.o'))], check=True)
    if a.platform == 'fixture':
        subprocess.run(['clang', '--target=x86_64-unknown-none-elf', *common, '-c',
                        str(ROOT/'tools/x86_64/tests/irq.S'), '-o', str(d/'irq.o')], check=True)
    env = dict(os.environ, RUSTFLAGS='-C relocation-model=pic -C code-model=small '
               '-C no-redzone=yes -C target-cpu=x86-64 -C target-feature=-avx,-avx2')
    subprocess.run(['cargo', 'build', '--offline', '--release', '--manifest-path',
                    str(ROOT/'tools/x86_64/math/Cargo.toml'), '--features', 'desktop',
                    '--target', 'x86_64-unknown-linux-gnu', '--target-dir', str(d/'rust')],
                   check=True, env=env)
    rust = d/'rust/x86_64-unknown-linux-gnu/release/libriplix_math.a'
    # Root every C ABI export, while retaining only reachable core/libm code.
    exports = sorted(elf_symbols(d/'gx.o')[1])
    exports = [x for x in exports if x.startswith('xd_')]
    subprocess.run([*elf_linker(), '-r', '--gc-sections', '-u', 'rcc_native_ext___rcc_host_math',
                    '-u', 'riplix_math_selftest', *[v for x in exports for v in ('-u', x)],
                    '--whole-archive', str(rust), '--no-whole-archive', '-o', str(d/'math.o')], check=True)
    objects = [x for x in sorted(d.glob('*.o')) if x.name != 'unsupported.o']
    defined, imports = {'kernel_readonly_end'}, set()
    for obj in objects:
        definitions, references = elf_symbols(obj)
        defined.update(definitions); imports.update(references)
    missing = sorted(imports-defined)
    if any(not x.startswith('rcc_native_') for x in missing):
        raise RuntimeError('unbound platform symbols: '+str(missing))
    asm = ['.text']
    for i, name in enumerate(missing):
        asm += [f'.globl {name}', name+':', f'lea unsupported_{i}(%rip),%rdi', 'jmp unsupported_service']
    asm += ['.section .rodata']
    asm += [f'unsupported_{i}: .asciz "unsupported service: {name}"' for i, name in enumerate(missing)]
    asm += ['.section .note.GNU-stack,"",@progbits']
    (d/'unsupported.S').write_text('\n'.join(asm)+'\n')
    subprocess.run(['clang', '--target=x86_64-unknown-none-elf', '-c', str(d/'unsupported.S'),
                    '-o', str(d/'unsupported.o')], check=True)
    objects.append(d/'unsupported.o')
    kernel = d/'kernel.elf'
    subprocess.run([*elf_linker(), '-pie', '--no-dynamic-linker', '-Bsymbolic', '--no-undefined',
                    '--gc-sections', '-T', str(ROOT/'tools/x86_64/boot/kernel.ld'),
                    '-o', str(kernel), *map(str, objects)], check=True)
    (d/'payload.S').write_text('.section .rdata,"dr"\n.globl kernel_payload_start,kernel_payload_end\n'
                              'kernel_payload_start:\n.incbin "'+str(kernel)+'"\nkernel_payload_end:\n')
    subprocess.run(['clang', '--target=x86_64-pc-windows-msvc', '-c', str(d/'payload.S'),
                    '-o', str(d/'payload.obj')], check=True)
    loader_objects = []
    for name in (('loader', 'pci-uefi') if a.platform == 'hp' else ('loader',)):
        source = ROOT/'tools/x86_64/boot'/(name+'.c'); target = d/(name+'.obj')
        records[str(source.relative_to(ROOT))] = sha(source)
        subprocess.run(['clang', '--target=x86_64-pc-windows-msvc', *common,
                        '-mno-stack-arg-probe', '-c', str(source), '-o', str(target)], check=True)
        loader_objects.append(target)
    efi = d/'esp/EFI/BOOT/BOOTX64.EFI'; efi.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([*linker(), '/subsystem:efi_application', '/entry:efi_main', '/nodefaultlib',
                    '/machine:x64', '/dynamicbase', '/base:0x100000', '/opt:ref', '/timestamp:0',
                    '/out:'+str(efi), *map(str, loader_objects), str(d/'payload.obj')], check=True)
    for name, digest in dependencies.items():
        if sha(ROOT/name) != digest:
            raise RuntimeError('source changed during build: '+name)
    report = dict(platform=a.platform, efi_sha256=sha(efi), kernel_sha256=sha(kernel),
                  c16_kernel_sha256=sha(guest), base_efi_sha256=sha(accepted),
                  base_inputs=inputs, rebuilt_sources=records, dependencies=dependencies, unsupported=missing,
                  kernel_ir={name:sha(Path(name)) for name in kernel_inputs+app_inputs},
                  filesystem_written=False, physical_acceptance=False)
    (d/'candidate.json').write_text(json.dumps(report, indent=2)+'\n')
    print('Native desktop candidate:', report['efi_sha256'], flush=True)

if __name__ == '__main__':
    main()
