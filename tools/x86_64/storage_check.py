"""Negative storage acceptance against the production QEMU driver."""
import hashlib
import json
import shutil
import subprocess


def run_storage_rejections(command, root, output, log, timeout, storage="virtio", root_identity=None):
    # These cases copy and hash the entire disk. Large sparse reservations use
    # check_image.py instead; never materialize a 30GiB sparse fixture here.
    if root.stat().st_size > 128 * 1024 * 1024:
        raise RuntimeError('storage rejection suite requires a small disposable image')
    directory = output / 'storage-errors'
    directory.mkdir(exist_ok=True)
    cases = {
        'missing': 'NVMe root disk missing' if storage == 'nvme' else 'modern virtio root disk missing',
        'readonly': 'root disk is read-only',
        'read-error': 'block test initial read',
        'write-error': 'block test partial I/O',
        'flush-error': 'block test restore/flush',
    }
    if storage == 'nvme':
        # QEMU's NVMe namespace may report backend RO only on a write command.
        cases['readonly'] = 'block test partial I/O'
    identity_cases = {
        'wrong-disk': 'root disk GUID mismatch',
        'wrong-partition': 'root partition GUID missing',
        'wrong-type': 'root partition type mismatch',
        'partition-readonly': 'root partition attributes unsupported',
        'partition-moved': 'root partition geometry mismatch',
        'root-format': 'root partition C16 V3 signature',
        'root-size': 'root partition filesystem bounds',
        'gpt-crc': 'GPT header CRC',
    } if root_identity else {}
    if root_identity:
        cases['read-error'] = 'GPT protective MBR read failed'
        cases.update(identity_cases)
    passed = []
    for case, expected in cases.items():
        current = list(command)
        serial = directory / f'{case}.log'
        serial.unlink(missing_ok=True)
        current[current.index('-serial') + 1] = f'file:{serial}'
        disk = directory / f'{case}.img'
        try:
            if case == 'missing':
                index = current.index('-blockdev')
                del current[index:index + 2]
                index = next(i for i, v in enumerate(current) if v.startswith('nvme,' if storage == 'nvme' else 'virtio-blk-pci,'))
                del current[index - 1:index + 1]
            else:
                shutil.copyfile(root, disk)
                if case in identity_cases:
                    from partition_image import damage_fixture
                    damage_fixture(disk, root_identity, case)
                before = hashlib.sha256(disk.read_bytes()).digest()
                index = current.index('-blockdev') + 1
                node = json.loads(current[index])
                node['file']['filename'] = str(disk)
                if case in ('readonly', 'gpt-crc'):
                    # UEFI may repair one damaged GPT header from the valid
                    # backup BEFORE our loader runs. Freeze that test input at
                    # the backend so the native parser sees the corruption.
                    node['read-only'] = node['file']['read-only'] = True
                elif case not in identity_cases:
                    event = {'read-error': 'read_aio', 'write-error': 'write_aio',
                             'flush-error': 'flush_to_disk'}[case]
                    node['file'] = {'driver': 'blkdebug', 'image': node['file'],
                                    'inject-error': [{'event': event, 'errno': 5}]}
                current[index] = json.dumps(node)
            result = subprocess.run(current, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
            text = serial.read_text(errors='replace') if serial.exists() else ''
            if result.returncode != 35 or f'BOOT_FAIL: {expected}' not in text or 'RIPLIX_X86_64_BOOT_OK' in text:
                raise RuntimeError(f'storage rejection {case} failed (QEMU {result.returncode}); inspect {serial}')
            if case not in ('missing', 'readonly') and case not in identity_cases and 'BLOCK_IO_ERROR' not in text:
                raise RuntimeError(f'{case} did not reach the driver error path; inspect {serial}')
            if case != 'missing' and hashlib.sha256(disk.read_bytes()).digest() != before:
                raise RuntimeError(f'{case} unexpectedly changed its test disk')
            passed.append(case)
            print(f'PASS: storage rejection {case}', flush=True)
        finally:
            disk.unlink(missing_ok=True)
    return passed
