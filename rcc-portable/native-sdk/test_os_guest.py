#!/usr/bin/env python3
"""Compile OS C sources in Riplix, then boot a host-assisted hybrid image.

The report distinguishes guest frontend output from the host native backend,
hardware objects, root userspace packages and EFI/image construction.
"""
import argparse
from datetime import datetime, timezone
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent / 'qbe'))
sys.path.insert(0, str(ROOT / 'tools/x86_64'))
from test_x86 import file, run_guest
from extract_guest import extract_files
from source_os import digest
from test_make_guest import save_record
from test_tools import discard_guest_disks
import boot
from qemu_check import run_shell_check
from platform_capsule import validate as validate_platform


def frontend_checkpoint(previous, inputs, manifest, ir):
    expected = {Path(r['output']).name for r in manifest['rows']}
    if (previous.get('frontend_sha256') != inputs['frontend_sha256']
            or previous.get('manifest_sha256') != inputs['manifest_sha256']
            or previous.get('build_inputs') != inputs['build_inputs']
            or previous.get('host_frontend_parity') is not True
            or set(previous.get('ir_sha256', {})) != expected
            or any(digest(ir / n) != h for n, h in previous['ir_sha256'].items())):
        raise ValueError('completed guest frontend checkpoint does not match')
    return previous['guest_commands']


def reference_outputs(source, manifest):
    assessment = json.loads((source / 'assessment.json').read_text())
    if (assessment['status'] != 'passed'
            or assessment['manifest_sha256'] != digest(source / 'manifest.json')):
        raise ValueError('host frontend assessment does not match the source bundle')
    outputs = {Path(r['output']).name: r['sha256'] for r in assessment['units']}
    if set(outputs) != {Path(r['output']).name for r in manifest['rows']}:
        raise ValueError('host frontend assessment has an incomplete output set')
    if any(digest(source / 'host-ir' / n) != h for n, h in outputs.items()):
        raise ValueError('host reference IR changed')
    return assessment


def hybrid_image(base, ir, manifest, output, backend_object=None, portable_linker=None):
    output.mkdir(parents=True, exist_ok=True)
    obj = output / 'rcc.o'
    command = [str(ROOT / 'target/release/rcc-native-object'), '--triple', 'x86_64-unknown-none']
    for row in manifest['rows']:
        if row['kind'] == 'kernel':
            command += ['--kernel', str(ir / Path(row['output']).name)]
    command += [str(ir / Path(row['output']).name) for row in manifest['rows'] if row['kind'] == 'application']
    if backend_object is None:
        subprocess.run([*command, '-o', str(obj)], check=True)
        backend = dict(kind='host-cranelift', path=command[0], sha256=digest(command[0]))
    else:
        shutil.copyfile(backend_object, obj)
        backend = dict(kind='supplied-object', path=str(backend_object), sha256=digest(backend_object))
    borrowed = validate_platform(base)
    script = base / 'kernel.ld' if (base / 'capsule.json').exists() else ROOT / 'tools/x86_64/boot/kernel.ld'
    kernel = output / 'kernel.elf'
    if portable_linker is None:
        subprocess.run([*boot.elf_linker(), '-pie', '--no-dynamic-linker', '-Bsymbolic',
                        '--no-undefined', '--gc-sections', '-T', str(script),
                        '-o', str(kernel), str(obj), *map(str, borrowed)], check=True)
    else:
        # The frozen profile boots a separate virtio root disk. Its legacy
        # embedded-root object is not referenced; host --gc-sections previously
        # discarded it. Omit that input explicitly rather than reading a 16 MiB
        # unused payload into the bounded guest linker heap.
        used = [p for p in borrowed if p.name != 'root.o']
        subprocess.run([str(portable_linker), '--kernel-elf', str(obj), *map(str, used),
                        '-o', str(kernel)], check=True)
    esp = output / 'esp'
    efi = esp / 'EFI/BOOT/BOOTX64.EFI'
    efi.parent.mkdir(parents=True, exist_ok=True)
    loader = base / 'loader.obj'
    if portable_linker is None:
        payload = output / 'payload.S'
        payload.write_text('.section .rdata,"dr"\n.globl kernel_payload_start,kernel_payload_end\n'
                           f'kernel_payload_start:\n.incbin "{kernel}"\nkernel_payload_end:\n')
        payload_obj = output / 'payload.obj'
        subprocess.run(['clang', '--target=x86_64-pc-windows-msvc', '-c', str(payload),
                        '-o', str(payload_obj)], check=True)
        subprocess.run([*boot.linker(), '/subsystem:efi_application', '/entry:efi_main',
                        '/nodefaultlib', '/machine:x64', '/dynamicbase', '/base:0x100000',
                        '/opt:ref', '/out:' + str(efi), str(loader), str(payload_obj)], check=True)
    else:
        subprocess.run([str(portable_linker), '--efi-loader', str(loader),
                        '--kernel-payload', str(kernel), '-o', str(efi)], check=True)
    if portable_linker is None:
        disk = boot.boot_disk(esp)
    else:
        disk = output / 'boot.img'
        subprocess.run([str(portable_linker), '--boot-media', str(efi), '-o', str(disk)], check=True)
    return disk, dict(
        backend=backend,
        hardware_objects={p.name: digest(p) for p in borrowed}, loader_sha256=digest(loader),
        root_seed_sha256=digest(base / 'root.img'), kernel_sha256=digest(kernel),
        linker_script_sha256=digest(script),
        capsule_sha256=digest(base / 'capsule.json') if (base / 'capsule.json').exists() else None)


def boot_hybrid(base, disk, output, timeout):
    code, variables = boot.firmware(argparse.Namespace(firmware_code=None, firmware_vars=None))
    root = output / 'root.img'
    shutil.copyfile(base / 'root.img', root)
    records = []
    for attempt in range(2):
        # Separate boot devices keep the firmware's preferred EFI path stable,
        # independent of discovery order of the writable guest root disk.
        vars_copy = output / ('vars' + str(attempt) + '.fd')
        shutil.copyfile(variables, vars_copy)
        serial = output / ('boot' + str(attempt) + '.log')
        serial.unlink(missing_ok=True)
        command = ['qemu-system-x86_64', '-machine', 'q35', '-accel', os.environ.get('RCC_TEST_ACCEL', 'tcg'),
                   '-cpu', 'host' if os.environ.get('RCC_TEST_ACCEL') == 'kvm' else 'qemu64',
                   '-m', '512M', '-smp', '1', '-no-reboot', '-nic', 'none', '-monitor', 'none',
                   '-display', 'none', '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04',
                   '-drive', f'if=pflash,format=raw,readonly=on,file={code}',
                   '-drive', f'if=pflash,format=raw,file={vars_copy}',
                   '-drive', f'if=none,id=bootdisk,format=raw,snapshot=on,file={disk}',
                   '-device', 'ide-hd,drive=bootdisk,bus=ide.0,bootindex=1',
                   '-blockdev', json.dumps(dict(driver='raw', **{'node-name': 'os-root'},
                                              file=dict(driver='file', filename=str(root)))),
                   '-device', 'virtio-blk-pci,drive=os-root,disable-legacy=on,num-queues=1,queue-size=8',
                   '-serial', 'file:' + str(serial)]
        with (output / ('qemu' + str(attempt) + '.log')).open('w') as log:
            result = run_shell_check(command, serial, log, timeout,
                                     output / ('boot' + str(attempt) + '.png'),
                                     persistent=True, restart=bool(attempt))
        text = serial.read_text(errors='replace')
        if result.returncode != 33 or 'RIPLIX_X86_64_BOOT_OK' not in text:
            raise RuntimeError('hybrid OS boot failed: ' + str(serial))
        records.append(dict(boot=attempt + 1, qemu_exit=result.returncode, serial_sha256=digest(serial)))
    return records


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--frontend', type=Path, required=True)
    p.add_argument('--base', type=Path, required=True, help='normal compiler-build boot/kernel/root directory')
    p.add_argument('--host-platform', type=Path, required=True, help='BOOT_TEST ROM/minimal/virtio platform objects')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--reuse-frontend', action='store_true',
                   help='reuse completed guest IR only after verifying its recorded inputs and all hashes')
    p.add_argument('--timeout', type=int, default=7200)
    a = p.parse_args()
    source, out = a.source.resolve(), a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = (out / '.build.lock').open('w')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    state_path = out / 'result.json'
    previous = json.loads(state_path.read_text()) if a.reuse_frontend else None
    state = dict(status='running', stage='guest-frontend', started_at=datetime.now(timezone.utc).isoformat(),
                 frontend_sha256=digest(a.frontend), manifest_sha256=digest(source / 'manifest.json'),
                 build_inputs={**{n: digest(a.base / n) for n in ['kernel.elf', 'boot.img', 'root.img']},
                               **{n: digest(ROOT / 'build/guest-native-assessment' / (n + '-x86.fxe'))
                                  for n in ['make', 'mkdir']}},
                 fully_guest_built=False)
    save_record(state_path, state)
    try:
        manifest = json.loads((source / 'manifest.json').read_text())
        for name, checksum in manifest['files'].items():
            if digest(source / name) != checksum:
                raise ValueError('frozen source changed: ' + name)
        assessment = reference_outputs(source, manifest)
        state['reference_frontend_sha256'] = assessment['frontend_sha256']
        state['reference_assessment_sha256'] = digest(source / 'assessment.json')
        if not (out / 'normal').exists():
            (out / 'normal').symlink_to(a.base.resolve(), target_is_directory=True)
        ir = out / 'guest-ir'
        if previous:
            rows = frontend_checkpoint(previous, state, manifest, ir)
            state['frontend_reused'] = True
        else:
            files = [file('/bin/rcc-c', a.frontend),
                     *[file('/bin/' + n, ROOT / 'build/guest-native-assessment' / (n + '-x86.fxe'))
                       for n in ['make', 'mkdir']]]
            files += [file('/tmp/os/' + n, source / n) for n in manifest['files']]
            commands = [('cd /tmp/os; make -r; echo OS_IR:$?', 'OS_IR:0')]
            root, rows = run_guest(out, 'os-source', files, commands, a.timeout)
            extract_files(root, ir, {Path(r['output']).name: '/tmp/os/' + r['output'] for r in manifest['rows']})
        # The reference frontend uses identical relative paths, includes and
        # macros. __FILE__ strings therefore compare exactly as well.
        for row in manifest['rows']:
            name = Path(row['output']).name
            if (ir / name).read_bytes() != (source / 'host-ir' / name).read_bytes():
                raise RuntimeError('guest/host frontend output differs: ' + row['source'])
        state.update(stage='host-backend', guest_source_units=len(manifest['rows']),
                     guest_commands=rows, host_frontend_parity=True,
                     ir_sha256={p.name: digest(p) for p in sorted(ir.glob('*.ir'))})
        save_record(state_path, state)
        disk, host = hybrid_image(a.host_platform.resolve(), ir, manifest, out / 'hybrid')
        state.update(stage='hybrid-boot', host_inputs=host, remaining_host_dependencies=manifest['host_dependencies'])
        save_record(state_path, state)
        boots = boot_hybrid(a.host_platform.resolve(), disk, out / 'hybrid', a.timeout)
        state.update(status='passed', stage='hybrid-boot', boots=boots)
        discard_guest_disks(out, 'os-source')
        print('Guest OS frontend, exact host parity and hybrid kernel two-boot acceptance passed', flush=True)
    except Exception as e:
        state.update(status='failed', error=str(e))
        raise
    finally:
        save_record(state_path, state)


if __name__ == '__main__':
    main()
