#!/usr/bin/env python3
"""Disposable QEMU acceptance using the actual x64 kernel and guest SSH."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
from types import SimpleNamespace
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools/x86_64'))
from boot import executable,firmware
from qemu_check import run_shell_check

def run(work,output,remote,port,files):
    kernel=ROOT/'build/ssh/x64-boot/normal'
    if not (kernel/'boot.img').exists():raise RuntimeError('build the private x64 kernel fixture first; see SSH README')
    probe=output/'mountcheck-x64.fxe'
    subprocess.run([sys.executable,str(ROOT/'fuse-x/cc.py'),str(Path(__file__).with_name('mountcheck.c')),
        '--static','--heap-profile','bounded','--linker',str(ROOT/'tools/x86_64/app_link.py'),
        '--build',str(ROOT/'build/ssh/x64/sdk'),'-o',str(probe)],check=True)
    files += [dict(name='/bin/sftp-mount',mode=0o100755,path=str(ROOT/'build/ssh/x64/sftp-mount.rxe')),
        dict(name='/bin/mountcheck',mode=0o100755,path=str(probe)),
        dict(name='/dev/urandom',mode=0o20444,device=0x104),dict(name='/dev/crypto',mode=0o20666,device=0x105)]
    for f in files:
        if f['name'].endswith('/config'):f['text']=f['text'].replace('Port 2222',f'Port {port}')
        if f['name'].endswith('/known_hosts'):f['text']=f['text'].replace(':2222',f':{port}')
    import json
    script="""import fs from 'node:fs';import {openImage} from './ports/fuzix/tools/image.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';import {atomicWrite} from './ports/fuzix/tools/disk.mjs';
const files=JSON.parse(process.argv[1]).map(f=>({...f,data:f.path?fs.readFileSync(f.path):Buffer.from(f.text||'')}));
atomicWrite(process.argv[3],installRoot(openImage(process.argv[2]),files));"""
    disk=work/'x64-root.img'
    subprocess.run(['node','--input-type=module','-e',script,json.dumps(files),str(kernel/'root.img'),str(disk)],cwd=ROOT,check=True)
    code,variables=firmware(SimpleNamespace(firmware_code=None,firmware_vars=None))
    variable_copy=work/'vars.fd';shutil.copyfile(variables,variable_copy)
    serial=output/'serial.log';serial.unlink(missing_ok=True)
    command=[executable('qemu-system-x86_64'),'-machine','q35','-accel','tcg','-cpu','max','-m','512M',
        '-smp','1','-no-reboot','-nic','none','-monitor','none','-display','none',
        '-drive',f'if=pflash,format=raw,readonly=on,file={code}',
        '-drive',f'if=pflash,format=raw,file={variable_copy}',
        '-drive',f'format=raw,snapshot=on,file={kernel/"boot.img"}',
        '-drive',f'if=none,id=rootdisk,format=raw,file={disk}',
        '-device','virtio-blk-pci,drive=rootdisk,disable-legacy=on',
        '-netdev','user,id=riplixnet,net=192.168.64.0/24,host=192.168.64.1,dns=192.168.64.3',
        '-device','virtio-net-pci,id=riplixnic,netdev=riplixnet,disable-legacy=on,mac=52:54:00:52:50:01',
        '-serial',f'file:{serial}']
    checks=[('mountcheck --setup',r'SFTP_SETUP_OK'),
        (f'sftp-mount --background fixture:{remote} /mnt/remote; echo STATUS:$?',r'STATUS:0'),
        ('mountcheck; echo STATUS:$?',r'SFTP_MOUNT_OK[\s\S]*STATUS:0'),
        ('sftp-mount --unmount 0; echo STATUS:$?',r'STATUS:0'),
        ('sftp-mount --status; echo STATUS:$?',r'STATUS:0')]
    with (output/'qemu.log').open('w') as log:
        result=run_shell_check(command,serial,log,300,output/'screen.png',persistent=True,
            debug_exit=False,checks_only=True,extra_checks=checks)
    if result.returncode:raise RuntimeError(f'QEMU exited {result.returncode}')
    if (remote/'original').read_bytes()!=b'after':raise RuntimeError('remote binary/rename result differs')
    sys.path.insert(0,str(ROOT/'fuse-x'));from test_support import check_root
    check_root(disk)
    print('PASS x64 QEMU: actual SFTP mount, binary I/O, append/truncate, atomic rename, open-file identity, concurrency and clean unmount/shutdown',flush=True)
