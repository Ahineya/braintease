#!/usr/bin/env python3
"""Accept an already-built USB image without rebuilding the compiler.

Works on a remote Linux/KVM machine or the local QEMU/TCG host. The VM gets
only regular images, copied firmware variables, one CPU and no network.
Partition-root acceptance creates its OWN disposable GPT fixture from a seed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
from contextlib import ExitStack
from boot import executable, firmware, validate_root_image
from qemu_check import run_shell_check


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--probe-disk', type=Path, help='regular NVMe image to attach read-only to a probe build')
    parser.add_argument('--probe-lba', type=int, choices=[512,4096], default=512)
    parser.add_argument('--probe-iommu', action='store_true', help='test Intel VT-d handoff as well')
    parser.add_argument('--partition-root-seed', type=Path,
                        help='create a disposable GPT root fixture from this regular C16 seed; test two boots')
    parser.add_argument('--root-identity', type=Path, help='fixture identity compiled into the partition-root image')
    parser.add_argument('--system-services', action='store_true')
    parser.add_argument('--network-user', action='store_true', help='attach disposable virtio NAT and accept supervised networking')
    parser.add_argument('--telnet-port', type=int, default=2324)
    parser.add_argument('--ssh-port', type=int, default=2325)
    parser.add_argument('--ssh-client-key', type=Path)
    parser.add_argument('--normal', action='store_true', help='production image without the test exit port')
    parser.add_argument('--accel', choices=['tcg', 'kvm'], default='tcg')
    parser.add_argument('--cpu')
    parser.add_argument('--memory', type=int, default=512, metavar='MiB',
                        help='VM RAM (default 512 MiB gives newer OVMF room for the embedded image)')
    parser.add_argument('--boot-transport', choices=['usb', 'sata'], default='usb')
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--qemu-data', type=Path)
    parser.add_argument('--firmware-code', type=Path)
    parser.add_argument('--firmware-vars', type=Path)
    parser.add_argument('--timeout', type=int, default=240)
    args = parser.parse_args()
    info = args.image.lstat()
    if not stat.S_ISREG(info.st_mode) or args.timeout <= 0:
        parser.error('image must be a regular file and timeout must be positive')
    if not 256 <= args.memory <= 65536:
        parser.error('--memory must be between 256 and 65536 MiB')
    if bool(args.partition_root_seed) != bool(args.root_identity):
        parser.error('--partition-root-seed and --root-identity are required together')
    if args.ssh_client_key and not (args.network_user and args.system_services):
        parser.error('SSH test requires --network-user --system-services')
    if any(not 1024<=p<=65535 for p in (args.telnet_port,args.ssh_port)) or args.telnet_port==args.ssh_port:
        parser.error('distinct localhost test ports required')
    if args.partition_root_seed and args.probe_disk:
        parser.error('partition-root and read-only-probe profiles are mutually exclusive')
    identity = outside_before = outside_after = None
    if args.partition_root_seed:
        from partition_image import load_identity, create_fixture, outside_fingerprint
        validate_root_image(args.partition_root_seed)
        identity = load_identity(args.root_identity)
    if args.probe_disk:
        from gpt_image import validate_probe_image, fingerprint
        validate_probe_image(args.probe_disk, args.probe_lba)
        probe_before = fingerprint(args.probe_disk)
    args.image = args.image.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    serial, report = args.output / 'serial.log', args.output / 'result.json'
    serial.unlink(missing_ok=True); report.unlink(missing_ok=True)
    code, variables = firmware(args)
    before = hashlib.sha256(args.image.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix='riplix-image-', dir=args.output) as temporary, ExitStack() as stack:
        vars_copy = Path(temporary) / 'vars.fd'
        shutil.copyfile(variables, vars_copy)
        command = [executable(args.qemu), '-machine', 'q35,kernel-irqchip=split' if args.probe_iommu and args.accel == 'kvm' else 'q35', '-accel', args.accel,
                   '-cpu', args.cpu or ('host' if args.accel == 'kvm' else 'qemu64'),
                   '-m', f'{args.memory}M', '-smp', '1', '-no-reboot', '-nic', 'none', '-monitor', 'none',
                   '-display', 'none', '-drive', f'if=pflash,format=raw,readonly=on,file={code}',
                   '-drive', f'if=pflash,format=raw,file={vars_copy}',
                   '-serial', f'file:{serial}']
        network_checks=None
        if args.network_user:
            command += ['-netdev', f'user,id=riplixnet,net=192.168.64.0/24,host=192.168.64.1,dns=192.168.64.3,hostfwd=tcp:127.0.0.1:{args.telnet_port}-192.168.64.250:23,hostfwd=tcp:127.0.0.1:{args.ssh_port}-192.168.64.250:22',
                        '-device','virtio-net-pci,id=riplixnic,netdev=riplixnet,disable-legacy=on,mac=52:54:00:52:50:01']
            from system_check import SystemCheck
            network_checks=stack.enter_context(SystemCheck(command,args.telnet_port,args.output,args.ssh_client_key,args.ssh_port))
        if args.boot_transport == 'usb':
            command += ['-device', 'qemu-xhci',
                        '-drive', f'if=none,id=bootusb,format=raw,snapshot=on,file={args.image}',
                        '-device', 'usb-storage,drive=bootusb,removable=on']
        else:
            command += ['-drive', f'format=raw,snapshot=on,file={args.image}']
        if args.probe_disk:
            command += ['-blockdev', json.dumps(dict(driver='raw', **{'node-name': 'probe-disk', 'read-only': True},
                         file=dict(driver='file', filename=str(args.probe_disk.resolve()), **{'read-only': True}))),
                        '-device', f'nvme,drive=probe-disk,serial=RIPLIXPROBE,logical_block_size={args.probe_lba},physical_block_size={args.probe_lba}']
        if args.probe_iommu:
            command += ['-device', 'intel-iommu,intremap=on,caching-mode=on']
        if identity:
            root = Path(temporary) / 'partition-root.img'
            create_fixture(root, args.partition_root_seed, identity)
            outside_before = outside_fingerprint(root, identity)
            lba = identity['sector_bytes']
            command += ['-blockdev', json.dumps(dict(driver='raw', **{'node-name': 'riplix-root'},
                         file=dict(driver='file', filename=str(root)),
                         cache=dict(direct=False, **{'no-flush': False}))),
                        '-device', f'nvme,drive=riplix-root,serial=RIPLIXFIXTURE,logical_block_size={lba},physical_block_size={lba}']
        if args.qemu_data:
            command += ['-L', str(args.qemu_data.resolve())]
        if not args.normal:
            command += ['-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']
        with (args.output / 'qemu.log').open('w') as log:
            result = run_shell_check(command, serial, log, args.timeout, args.output / 'screen.png',
                                     debug_exit=not args.normal, persistent=bool(identity), storage='nvme',system_services=args.system_services,network_checks=network_checks)
            if identity:
                if result.returncode != (0 if args.normal else 33):
                    raise RuntimeError('first partition-root boot failed')
                first = serial.read_text(errors='replace')
                (args.output / 'storage-first-boot.log').write_text(first)
                if outside_fingerprint(root, identity) != outside_before:
                    raise RuntimeError('first production boot changed bytes outside the root partition')
                serial.unlink()
                result = run_shell_check(command, serial, log, args.timeout, args.output / 'restart-screen.png',
                                         debug_exit=not args.normal, persistent=True, restart=True, storage='nvme',system_services=args.system_services,network_checks=network_checks)
                second = serial.read_text(errors='replace')
                (args.output / 'storage-second-boot.log').write_text(second)
                serial.write_text(first + '\n--- FRESH QEMU PROCESS ---\n' + second)
                outside_after = outside_fingerprint(root, identity)
                if outside_after != outside_before:
                    raise RuntimeError('second production boot changed bytes outside the root partition')
    text = serial.read_text(errors='replace')
    expected = ['UEFI_RELOCATION_OK', 'RSDP_HANDOFF', 'ACPI_OK', 'IOAPIC_MASKED', 'APIC_IRQ_OK',
                'ROOT_STORAGE nvme persistent' if identity else 'ROOT_STORAGE ram',
                'RIPLIX_X86_64_BOOT_OK', 'INDEPENDENT_IMAGES_OK',
                'timer checks passed', 'filesystem synced; session stopped']
    if args.probe_disk:
        if fingerprint(args.probe_disk) != probe_before:
            raise RuntimeError('read-only virtual probe disk changed')
        expected += ['DMA_IDENTITY_OK', 'NVME_PROBE_READONLY_OK', 'NVME_PROBE_IO',
                     'writes=0 flushes=0', 'NVME_PROBE_QUIESCED']
        if args.probe_iommu:
            expected += ['DMAR_HANDOFF']
    if not args.normal:
        expected += ['IRQ_CONTEXT_OK']
    if identity:
        expected += ['ROOT_PARTITION_OK', 'NVME_BLK_OK', 'BLOCK_IO_OK', 'PERSISTENCE_SECOND_BOOT_OK']
    after = hashlib.sha256(args.image.read_bytes()).hexdigest()
    if result.returncode != (0 if args.normal else 33) or before != after or \
            any(marker not in text for marker in expected) or 'BOOT_FAIL' in text or 'EXCEPTION' in text:
        raise RuntimeError('image acceptance failed; inspect output transcripts')
    report.write_text(json.dumps(dict(status='passed', image=str(args.image), sha256=after,
                                     accelerator=args.accel, cpu=args.cpu or ('host' if args.accel == 'kvm' else 'qemu64'),
                                     memory_mib=args.memory,
                                     production=args.normal, boot_transport=args.boot_transport,
                                     probe_iommu=args.probe_iommu,
                                     kernel_irqchip="split" if args.probe_iommu and args.accel == "kvm" else "auto",
                                     probe_disk_fingerprint=probe_before if args.probe_disk else None,
                                     root_identity=identity, persistence_boots=2 if identity else 0,
                                     outside_partition_before=outside_before, outside_partition_after=outside_after,
                                     checks=expected), indent=2) + '\n')
    print(f'PASS: {args.accel} {args.boot_transport} image, production={args.normal}, sha256={after}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        raise SystemExit(f'x86_64 image: {error}') from None
