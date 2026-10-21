#!/usr/bin/env python3
"""Reproduce late HP MMIO mappings in disposable QEMU guests, without RF.

Reuses accepted C16 objects/apps, rebuilding only a compile-time instrumented
native runtime. An old-guard control must fail with two pages; a real unowned
allocation must still fail with one page; the corrected balance must reboot.
Never opens a physical device or copies a physical filesystem.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
from boot import HERE, ROOT, elf_linker, linker, executable, firmware
from gpt_image import load_capture, write_image
from qemu_check import run_shell_check
from usb_image import boot_disk


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--accepted', type=Path, required=True)
    p.add_argument('--capture', type=Path, required=True)
    p.add_argument('--identity', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--timeout', type=int, default=180)
    a = p.parse_args()
    base, out = a.accepted.resolve(), a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'result.json').unlink(missing_ok=True)
    code, variables = firmware(SimpleNamespace(firmware_code=None, firmware_vars=None))
    defines = ['BOOT_NETWORK', 'BOOT_WIFI_LIVE', 'BOOT_WIFI_USERLAND', 'BOOT_WIFI_BRINGUP',
               'BOOT_WIFI_PNVM', 'BOOT_WIFI_SCAN', 'BOOT_WIFI_PREPARE', 'BOOT_WIFI_AUTH',
               'BOOT_WIFI_ASSOC', 'BOOT_WIFI_WPA2', 'BOOT_WIFI_DATA', 'BOOT_ROOT_PARTITION',
               'BOOT_MATH', 'BOOT_FONT_ATLAS', 'RIPLIX_BOOT', 'PERSISTENT_ROOT', 'ROOT_NVME']
    flags = ['--target=x86_64-unknown-none-elf', '-ffreestanding', '-fno-builtin',
             '-fno-stack-protector', '-mno-red-zone', '-mgeneral-regs-only', '-O2',
             '-Wall', '-Wextra', '-Werror', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
             '-ffunction-sections', '-fdata-sections', '-fpie', '-DCONSOLE_SCALE=0', '-DBOOT_FAULT=0',
             *[f'-D{x}=1' for x in defines], '-I', str(base), '-I', str(HERE / 'boot')]
    source = (HERE / 'boot/runtime.c').read_text()
    source = source.replace('"../tests/page-ownership.inc"', '"' + str(HERE / 'tests/page-ownership.inc') + '"')
    capture = load_capture(a.capture)
    ident = json.loads(a.identity.read_text())
    first = ident['first_lba'] * ident['sector_bytes']
    results = {}
    with tempfile.TemporaryDirectory(prefix='ownership-', dir=out) as temporary:
        work = Path(temporary)
        for name, mode, expected in [('old-guard', 1, 2), ('real-leak', 2, 1), ('owned-map', 1, None)]:
            d = work / name
            d.mkdir(mode=0o700)
            runtime = source
            if name == 'old-guard':
                assert runtime.count('page_available() + page_mapping_owned()') == 2
                runtime = runtime.replace('page_available() + page_mapping_owned()', 'page_available()')
            (d / 'runtime.c').write_text(runtime)
            subprocess.run(['clang', *flags, f'-DBOOT_PAGE_OWNERSHIP_TEST={mode}', '-c',
                            str(d / 'runtime.c'), '-o', str(d / 'runtime.o')], check=True)
            objects = [d / 'runtime.o', *[x for x in sorted(base.glob('*.o')) if x.name != 'runtime.o']]
            kernel = d / 'kernel.elf'
            subprocess.run([*elf_linker(), '-pie', '--no-dynamic-linker', '-Bsymbolic', '--no-undefined',
                            '--gc-sections', '-T', str(HERE / 'boot/kernel.ld'), '-o', str(kernel),
                            *map(str, objects)], check=True)
            payload = d / 'payload.S'
            payload.write_text('.section .rdata,"dr"\n.globl kernel_payload_start,kernel_payload_end\n'
                               'kernel_payload_start:\n.incbin "' + str(kernel) + '"\nkernel_payload_end:\n')
            subprocess.run(['clang', '--target=x86_64-pc-windows-msvc', '-c', str(payload),
                            '-o', str(d / 'payload.obj')], check=True)
            efi = d / 'esp/EFI/BOOT/BOOTX64.EFI'
            efi.parent.mkdir(parents=True)
            subprocess.run([*linker(), '/subsystem:efi_application', '/entry:efi_main', '/nodefaultlib',
                            '/machine:x64', '/dynamicbase', '/base:0x100000', '/opt:ref', '/timestamp:0',
                            f'/out:{efi}', str(base / 'loader.obj'), str(d / 'payload.obj')], check=True)
            image = boot_disk(d / 'esp')
            root = d / 'disk.img'
            write_image(root, capture)
            with (base / 'root.img').open('rb') as src, root.open('r+b') as dst:
                at = first
                while chunk := src.read(1024 ** 2):
                    if any(chunk):
                        dst.seek(at)
                        dst.write(chunk)
                    at += len(chunk)
            var = d / 'vars.fd'
            shutil.copyfile(variables, var)
            serial = out / f'{name}.serial.log'
            serial.unlink(missing_ok=True)
            command = [executable('qemu-system-x86_64'), '-machine', 'q35', '-accel', 'tcg', '-cpu', 'max',
                       '-m', '1024M', '-smp', '1', '-no-reboot', '-nic', 'none', '-monitor', 'none',
                       '-display', 'none', '-drive', f'if=pflash,format=raw,readonly=on,file={code}',
                       '-drive', f'if=pflash,format=raw,file={var}', '-serial', f'file:{serial}',
                       '-device', 'qemu-xhci', '-drive', f'if=none,id=bootusb,format=raw,snapshot=on,file={image}',
                       '-device', 'usb-storage,drive=bootusb,removable=on', '-drive',
                       f'if=none,id=root,format=raw,file={root}', '-device', 'nvme,drive=root,serial=OWNERSHIPTEST']
            error = None
            with (out / f'{name}.qemu.log').open('w') as log:
                try:
                    run_shell_check(command, serial, log, a.timeout, out / f'{name}.png',
                                    persistent=True, debug_exit=False, storage='nvme', system_services=True,
                                    checks_only=True, exit_command='reboot')
                except RuntimeError as failure:
                    error = str(failure)
            text = serial.read_text()
            assert 'LATE_DEVICE_MAP_TEST_OK retained=2 repeated=0' in text
            assert 'filesystem synced; session stopped' in text
            assert 'EXCEPTION vector=' not in text
            if expected is not None:
                assert error and f'BOOT_FAIL: context physical page leak 0x{expected:016x}' in text, (name, error)
                assert 'PLATFORM_REBOOT' not in text
            else:
                assert error is None and 'CONTEXT_PAGES_OK' in text and 'PLATFORM_REBOOT' in text, error
                assert 'BOOT_FAIL' not in text
            results[name] = {'status': 'passed', 'expected_leak_pages': expected,
                             'test_efi_sha256': hashlib.sha256(efi.read_bytes()).hexdigest()}
            (out / 'kernel-memory.bin').unlink(missing_ok=True)
            shutil.rmtree(d)
            print(f'PASS: {name}', flush=True)
    result = {'status': 'passed', 'cases': results, 'rf_emulated': False,
              'production_efi_sha256': hashlib.sha256((base / 'esp/EFI/BOOT/BOOTX64.EFI').read_bytes()).hexdigest(),
              'source_sha256': {str(x.relative_to(ROOT)): hashlib.sha256(x.read_bytes()).hexdigest()
                                for x in [HERE / 'boot/kernel.c', HERE / 'boot/runtime.c', HERE / 'tests/page-ownership.inc']}}
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
