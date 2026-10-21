#!/usr/bin/env python3
"""Assess per-unit OS IR lowering in Riplix; this is not kernel linking.

The application-oriented metadata/runtime bindings still need a kernel mode.
These QBE files are compared with host output, not substituted into a boot image.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent / 'qbe'))
from test_x86 import file, run_guest
from extract_guest import extract_files
from source_os import digest
from test_tools import discard_guest_disks
from test_make_guest import save_record


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--os-build', type=Path, required=True, help='accepted test_os_guest output directory')
    p.add_argument('--lowerer', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--timeout', type=int, default=1800)
    a = p.parse_args()
    original, out = a.os_build.resolve(), a.output.resolve()
    record = json.loads((original / 'result.json').read_text())
    if record['status'] != 'passed' or record.get('host_frontend_parity') is not True:
        raise ValueError('OS frontend build must have passed')
    names = sorted(record['ir_sha256'])
    for n in names:
        if Path(n).name != n or digest(original / 'guest-ir' / n) != record['ir_sha256'][n]:
            raise ValueError('accepted OS IR changed: ' + n)
    if out.exists():
        raise ValueError('use a new OS lowering output directory')
    out.mkdir(parents=True)
    (out / 'normal').symlink_to((original / 'normal').resolve(), target_is_directory=True)
    state = dict(status='running', scope='per-unit unlinked QBE assessment; not kernel execution',
                 os_result_sha256=digest(original / 'result.json'), lowerer_sha256=digest(a.lowerer),
                 host_lowerer_sha256=digest(ROOT / 'rcc-portable/build/rcc-qbe'), source_units=len(names))
    save_record(out / 'result.json', state)
    try:
        files = [file('/bin/rcc-qbe', a.lowerer)]
        files += [file('/' + n, original / 'guest-ir' / n) for n in names]
        commands = [('rcc-qbe --relocatable --no-start /' + n + ' -o /tmp/'
                     + Path(n).with_suffix('.ssa').name + '; echo LOWER_' + str(i) + ':$?',
                     'LOWER_' + str(i) + ':0') for i, n in enumerate(names)]
        disk, rows = run_guest(out, 'os-lowering', files, commands, a.timeout)
        guest = out / 'guest-ssa'
        extract_files(disk, guest, {Path(n).with_suffix('.ssa').name:
                                   '/tmp/' + Path(n).with_suffix('.ssa').name for n in names})
        reference = out / 'host-ssa'
        reference.mkdir()
        for n in names:
            dest = Path(n).with_suffix('.ssa').name
            subprocess.run([str(ROOT / 'rcc-portable/build/rcc-qbe'), '--relocatable', '--no-start',
                            str(original / 'guest-ir' / n), '-o', str(reference / dest)], check=True)
            if (guest / dest).read_bytes() != (reference / dest).read_bytes():
                raise RuntimeError('OS QBE host/guest lowering differs: ' + n)
        state.update(status='passed', commands=rows, host_parity=True,
                     outputs={p.name: digest(p) for p in sorted(guest.glob('*.ssa'))})
        discard_guest_disks(out, 'os-lowering')
        print('Per-unit OS QBE lowering and host parity passed; kernel linking remains separate', flush=True)
    except Exception as e:
        state.update(status='failed', error=str(e))
        raise
    finally:
        save_record(out / 'result.json', state)


if __name__ == '__main__':
    main()
