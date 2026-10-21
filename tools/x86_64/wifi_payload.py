"""Package the exact recovery API89 firmware used for startup/PNVM experiments.

No download, device access or credentials. Optional PNVM uses the matching snapshot.
The firmware and all redistribution notices are embedded in the same retained
ELF section, so copying only BOOTX64.EFI still carries the license materials.
"""
import hashlib
import json
from pathlib import Path
from wifi_firmware import inspect

SHA256 = '9984b550d78ede691433f6de9d19c16cbf85e1e2806ef42904e790853d2a4227'
BYTES = 1752712
PNVM_BYTES = 55096
PNVM_SHA256 = '067f24f8d601e4f0238d53b2a769f5bd58aa2619c780a334d9da83b514dc6199'


def prepare(directory, source, pnvm=None):
    source = Path(source)
    with source.open('rb') as stream:
        data = stream.read(BYTES + 1)
    if len(data) != BYTES or hashlib.sha256(data).hexdigest() != SHA256:
        raise RuntimeError('AX211 startup needs the pinned recovery ma-b0-gf-a0-89 firmware; see hardware/hp-wifi.txt')
    pnvm_assembly = ''
    if pnvm is not None:
        with Path(pnvm).open('rb') as stream:
            pnvm_data = stream.read(PNVM_BYTES + 1)
        if len(pnvm_data) != PNVM_BYTES or hashlib.sha256(pnvm_data).hexdigest() != PNVM_SHA256:
            raise RuntimeError('AX211 PNVM needs the matching hash-pinned recovery snapshot')
        pnvm_path = directory / 'wifi.pnvm'
        pnvm_path.write_bytes(pnvm_data)
        pnvm_assembly = ('.balign 4\n.globl wifi_pnvm_start,wifi_pnvm_end\n'
                         f'wifi_pnvm_start:\n.incbin {json.dumps(str(pnvm_path))}\nwifi_pnvm_end:\n')
    report = inspect(data)
    report['pnvm_sha256'] = PNVM_SHA256 if pnvm is not None else None
    here = Path(__file__).resolve().parent
    license_text = ((here / 'boot/wifi-LICENSE.txt').read_text() + '\nIntel firmware notice:\n' +
                    (here / 'boot/wifi-firmware-LICENSE.txt').read_text())
    payload = directory / 'wifi.ucode'
    payload.write_bytes(data)
    notice = directory / 'wifi-notices.txt'
    notice.write_text(license_text)
    report['hardware_status'] = 'startup/ALIVE plus optional PNVM experiment; NVM/calibration/scan/WPA2/link pending'
    (directory / 'wifi-firmware.json').write_text(json.dumps(report, indent=2) + '\n')
    assembly = directory / 'wifi-payload.S'
    assembly.write_text('.section .rodata.wifi,"a",@progbits\n.balign 4096\n'
                        '.globl wifi_payload_start,wifi_payload_end\n'
                        f'wifi_payload_start:\n.incbin {json.dumps(str(payload))}\nwifi_payload_end:\n' +
                        pnvm_assembly + f'.incbin {json.dumps(str(notice))}\n'
                        '.section .note.GNU-stack,"",@progbits\n')
    return assembly
