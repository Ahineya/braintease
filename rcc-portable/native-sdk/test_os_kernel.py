#!/usr/bin/env python3
"""Lower accepted OS IR with portable C tools and boot against a frozen platform.

This removes Cranelift, LLD and media tooling from C16 kernel image generation.
Native platform objects and root packages remain explicit host bootstrap inputs.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from source_os import digest
from test_make_guest import save_record
from test_os_guest import hybrid_image, boot_hybrid
from platform_capsule import validate as validate_platform
from test_tools import discard_guest_disks
from extract_guest import extract_files
from test_x86 import file, run_guest


def lowering_checkpoint(previous, state, artifact):
    if (previous.get('status') != 'passed' or previous.get('lowerer_parity') is not True
            or any(previous.get(k) != state[k] for k in
                   ['source_manifest_sha256', 'os_result_sha256', 'build_base'])
            or previous.get('guest_tools', {}).get('rcc-qbe', {}).get('sha256')
               != state['guest_tools']['rcc-qbe']['sha256']
            or previous.get('outputs', {}).get('kernel.ssa') != digest(artifact)):
        raise ValueError('completed kernel lowering checkpoint does not match')
    return previous['lowering']


def guest_image(a, manifest, out, state):
    tools = a.guest_tools.resolve()
    names = ['rcc-qbe', 'qbe', 'rcc-x64-as', 'rcc-x64-ld']
    state['guest_tools'] = {n: dict(path=str(tools / (n + '.fxe')),
                                  sha256=digest(tools / (n + '.fxe'))) for n in names}
    state['build_base'] = {n: digest(a.base / n) for n in ['kernel.elf', 'boot.img', 'root.img']}
    state['execution'] = 'portable backend, assembler, ELF/EFI link and media construction in Riplix'
    (out / 'normal').symlink_to(a.base.resolve(), target_is_directory=True)
    save_record(out / 'result.json', state)
    files = [file('/bin/rcc-qbe', tools / 'rcc-qbe.fxe')]
    files += [file('/' + Path(r['output']).name, a.os_build / 'guest-ir' / Path(r['output']).name)
              for r in manifest['rows']]
    args = []
    for r in manifest['rows']:
        if r['kind'] == 'kernel':
            args += ['--kernel', '/' + Path(r['output']).name]
    args += ['/' + Path(r['output']).name for r in manifest['rows'] if r['kind'] == 'application']
    script = out / 'lower.sh'
    script.write_text('set -e\nrcc-qbe ' + ' '.join(args) + ' -o /tmp/kernel.ssa\n')
    files.append(file('/lower.sh', script))
    commands = [('sh /lower.sh; echo KERNEL_LOWER:$?', 'KERNEL_LOWER:0')]
    state['lowering_script_sha256'] = digest(script)
    artifacts = out / 'guest'
    if a.reuse_lowering:
        previous = json.loads((a.reuse_lowering / 'result.json').read_text())
        artifact = a.reuse_lowering / 'guest/kernel.ssa'
        rows = lowering_checkpoint(previous, state, artifact)
        artifacts.mkdir()
        shutil.copyfile(artifact, artifacts / 'kernel.ssa')
        state['reused_lowering'] = digest(a.reuse_lowering / 'result.json')
    else:
        disk, rows = run_guest(out, 'kernel-lowering', files, commands, a.timeout)
        extract_files(disk, artifacts, {'kernel.ssa': '/tmp/kernel.ssa'})
    host = ROOT / 'rcc-portable/build/rcc-qbe'
    reference = out / 'reference.ssa'
    host_args = [str(a.os_build / 'guest-ir' / t[1:]) if t.startswith('/u') else t for t in args]
    subprocess.run([str(host), *host_args, '-o', str(reference)], check=True, timeout=a.timeout)
    if (artifacts / 'kernel.ssa').read_bytes() != reference.read_bytes():
        raise RuntimeError('combined kernel QBE differs between host and guest')
    state.update(stage='guest-assembly-link-media', lowerer_parity=True, lowering=rows,
                 outputs={'kernel.ssa': digest(artifacts / 'kernel.ssa')})
    save_record(out / 'result.json', state)
    discard_guest_disks(out, 'kernel-lowering')
    if a.lowering_only:
        return
    platform = a.host_platform.resolve()
    objects = [p for p in validate_platform(platform) if p.name != 'root.o']
    files = [file('/bin/' + n, tools / (n + '.fxe')) for n in names[1:]]
    files += [file('/kernel.ssa', artifacts / 'kernel.ssa'), file('/loader.obj', platform / 'loader.obj')]
    files += [file('/p/' + p.name, p) for p in objects]
    commands = [
        ('qbe -t amd64_sysv -o /tmp/kernel.s /kernel.ssa; echo QBE:$?', 'QBE:0'),
        # Reclaim the consumed input so all intermediate and final artifacts
        # fit the C16 filesystem's bounded block-number space.
        ('echo consumed >/kernel.ssa; echo RECLAIM:$?', 'RECLAIM:0'),
        ('rcc-x64-as /tmp/kernel.s -o /tmp/kernel.o; echo AS:$?', 'AS:0'),
        ('rcc-x64-ld --kernel-elf /tmp/kernel.o ' + ' '.join('/p/' + p.name for p in objects)
         + ' -o /tmp/kernel.elf; echo ELF:$?', 'ELF:0'),
        ('rcc-x64-ld --efi-loader /loader.obj --kernel-payload /tmp/kernel.elf -o /tmp/BOOTX64.EFI; echo EFI:$?', 'EFI:0'),
        ('rcc-x64-ld --boot-media /tmp/BOOTX64.EFI -o /tmp/boot.img; echo MEDIA:$?', 'MEDIA:0')]
    script = out / 'image.sh'
    script.write_text('set -e\n' + '\n'.join(c[0] for c in commands) + '\n')
    files.append(file('/image.sh', script))
    state['image_script_sha256'] = digest(script)
    disk, rows = run_guest(out, 'kernel-image', files,
                           [('sh /image.sh; echo IMAGE:$?', 'IMAGE:0')], a.timeout)
    outputs = ['kernel.s', 'kernel.o', 'kernel.elf', 'BOOTX64.EFI', 'boot.img']
    extract_files(disk, artifacts, {n: '/tmp/' + n for n in outputs})
    # Compare link/packaging on the actual guest-allocated object. QBE's legal
    # equal-cost register choices may differ from host libc qsort's choices.
    ref = out / 'reference'
    ref.mkdir()
    linker = ROOT / 'rcc-portable/build/rcc-x64-ld'
    subprocess.run([str(linker), '--kernel-elf', str(artifacts / 'kernel.o'), *map(str, objects),
                    '-o', str(ref / 'kernel.elf')], check=True)
    subprocess.run([str(linker), '--efi-loader', str(platform / 'loader.obj'), '--kernel-payload',
                    str(ref / 'kernel.elf'), '-o', str(ref / 'BOOTX64.EFI')], check=True)
    subprocess.run([str(linker), '--boot-media', str(ref / 'BOOTX64.EFI'), '-o', str(ref / 'boot.img')], check=True)
    for n in ['kernel.elf', 'BOOTX64.EFI', 'boot.img']:
        if (artifacts / n).read_bytes() != (ref / n).read_bytes():
            raise RuntimeError('guest kernel image/link parity differs: ' + n)
    state.update(stage='guest-image-boot', guest_image_built=True, linker_media_parity=True,
                 image_commands=rows, outputs={p.name: digest(p) for p in artifacts.iterdir()},
                 platform_inputs={p.name: digest(p) for p in [*objects, platform / 'loader.obj', platform / 'root.img']},
                 unused_platform_objects=['root.o (separate virtio root)'])
    save_record(out / 'result.json', state)
    discard_guest_disks(out, 'kernel-image')
    image = out / 'image'
    image.mkdir()
    state['boots'] = boot_hybrid(platform, artifacts / 'boot.img', image, a.timeout)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--os-build', type=Path, required=True)
    p.add_argument('--host-platform', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--timeout', type=int, default=600)
    p.add_argument('--guest-tools', type=Path, help='rcc-qbe/qbe/rcc-x64-as/rcc-x64-ld.fxe; run backend in Riplix')
    p.add_argument('--base', type=Path, help='compiler-test kernel/boot/root seed, required with --guest-tools')
    p.add_argument('--lowering-only', action='store_true', help='accept combined guest kernel lowering without building an image')
    p.add_argument('--reuse-lowering', type=Path, help='verify and reuse an accepted guest kernel lowering checkpoint')
    a = p.parse_args()
    if a.guest_tools and not a.base:
        p.error('--guest-tools requires --base')
    if (a.lowering_only or a.reuse_lowering) and not a.guest_tools:
        p.error('guest lowering options require --guest-tools')
    out = a.output.resolve()
    if out.exists():
        raise ValueError('use a new kernel build output directory')
    manifest = json.loads((a.source / 'manifest.json').read_text())
    original = json.loads((a.os_build / 'result.json').read_text())
    if (original['status'] != 'passed' or not original.get('host_frontend_parity')
            or original['manifest_sha256'] != digest(a.source / 'manifest.json')):
        raise ValueError('OS frontend checkpoint does not match')
    expected = {Path(r['output']).name for r in manifest['rows']}
    if set(original['ir_sha256']) != expected:
        raise ValueError('OS frontend checkpoint is incomplete')
    for n, h in original['ir_sha256'].items():
        if Path(n).name != n or digest(a.os_build / 'guest-ir' / n) != h:
            raise ValueError('OS IR changed: ' + n)
    out.mkdir(parents=True)
    state = dict(status='running', stage='portable-lowering', fully_guest_built=False,
                 execution='portable backend tools running on host',
                 source_manifest_sha256=digest(a.source / 'manifest.json'),
                 os_result_sha256=digest(a.os_build / 'result.json'))
    save_record(out / 'result.json', state)
    try:
        if a.guest_tools:
            guest_image(a, manifest, out, state)
            state.update(status='passed', stage='guest-kernel-lowering' if a.lowering_only else 'guest-kernel-image-boot')
            print('Combined guest kernel QBE and host parity passed' if a.lowering_only else
                  'Guest-built kernel ELF, EFI and boot image passed parity and two boots', flush=True)
            return
        tools = ROOT / 'rcc-portable/build'
        command = [str(tools / 'rcc-qbe')]
        for r in manifest['rows']:
            if r['kind'] == 'kernel':
                command += ['--kernel', str((a.os_build / 'guest-ir' / Path(r['output']).name).resolve())]
        command += [str((a.os_build / 'guest-ir' / Path(r['output']).name).resolve())
                    for r in manifest['rows'] if r['kind'] == 'application']
        ssa, assembly, obj = [out / ('kernel' + s) for s in ['.ssa', '.s', '.o']]
        steps = [command + ['-o', str(ssa)],
                 [str(tools / 'qbe-host/qbe'), '-t', 'amd64_sysv', '-o', str(assembly), str(ssa)],
                 [str(tools / 'rcc-x64-as'), str(assembly), '-o', str(obj)]]
        state['tools'] = {str(Path(c[0]).relative_to(ROOT)): digest(c[0]) for c in steps}
        for i, c in enumerate(steps):
            with (out / ('step' + str(i) + '.log')).open('w') as log:
                subprocess.run(c, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=a.timeout)
        state.update(stage='link-and-boot', outputs={p.name: digest(p) for p in [ssa, assembly, obj]})
        save_record(out / 'result.json', state)
        linker = tools / 'rcc-x64-ld'
        disk, inputs = hybrid_image(a.host_platform.resolve(), a.os_build / 'guest-ir', manifest,
                                    out / 'image', backend_object=obj, portable_linker=linker)
        inputs['kernel_linker'] = dict(path=str(linker), sha256=digest(linker))
        inputs['unused_platform_objects'] = ['root.o (virtio root profile; no embedded root)']
        state['host_inputs'] = inputs
        state['boots'] = boot_hybrid(a.host_platform.resolve(), disk, out / 'image', a.timeout)
        state.update(status='passed', stage='portable-kernel-boot')
        print('Portable C/QBE/assembler kernel passed two-boot OS acceptance', flush=True)
    except Exception as e:
        state.update(status='failed', error=str(e))
        raise
    finally:
        save_record(out / 'result.json', state)


if __name__ == '__main__':
    main()
