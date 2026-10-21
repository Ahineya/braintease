#!/usr/bin/env python3
"""Exercise production AX211 code with fake DMA/MMIO; never access a radio."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from boot import ROOT, HERE
from wifi_payload import SHA256, PNVM_SHA256

MODULES=('image','pnvm','nvm','scan','security','prepare','auth','assoc','wpa','wpa-transport',
         'ether','data','michael','live','rekey','restart','crypto','sha1','aes','transport')
SUITES=('start','pnvm','nvm','scan','native','native-userland','prepare','auth','audit','assoc','crypto','wpa',
        'wpa-transport','ether','data','group','live','live-diag','rekey','restart','control','policy','daemon')
NO_FIRMWARE={'native','native-userland','audit','crypto','wpa','ether','group','restart','control','policy','daemon'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware',type=Path,required=True)
    parser.add_argument('--pnvm',type=Path,required=True)
    parser.add_argument('--output',type=Path,default=ROOT/'build/x86_64-wifi-host')
    parser.add_argument('--cc',default='clang')
    parser.add_argument('--sysroot',type=Path)
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    report=out/'result.json';report.unlink(missing_ok=True)
    for p,sha in [(args.firmware,SHA256),(args.pnvm,PNVM_SHA256)]:
        if hashlib.sha256(p.read_bytes()).hexdigest()!=sha:parser.error('expected pinned recovery firmware/PNVM')
    modules=[HERE/'boot'/f'wifi-{n}.c' for n in MODULES]
    flags=['-std=c11','-D_POSIX_C_SOURCE=200809L','-O1','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all']
    if args.sysroot:flags+=['-isysroot',str(args.sysroot.resolve())]
    checks={}
    for name in (*SUITES,'net-ring','network'):
        source=HERE/'tests'/((f'wifi-{"native" if name=="native-userland" else name}' if name in SUITES else name)+'.c')
        binary=out/f'{name}-host';log=out/f'{name}.log'
        with log.open('w') as stream:
            subprocess.run([args.cc,*flags,*(['-DBOOT_WIFI_LIVE=1'] if name.startswith('native') else []),
                            *(['-DBOOT_WIFI_USERLAND=1'] if name=='native-userland' else []),
                            str(source),*(map(str,modules) if name in SUITES else []),'-o',str(binary)],
                           stdout=stream,stderr=subprocess.STDOUT,check=True)
            parameters=[] if name in NO_FIRMWARE or name not in SUITES else [str(args.firmware.resolve())]
            if parameters and name!='start':parameters+=[str(args.pnvm.resolve())]
            subprocess.run([str(binary),*parameters],stdout=stream,stderr=subprocess.STDOUT,check=True,timeout=90)
        checks[name]=log.read_text().strip();print(checks[name].splitlines()[-1],flush=True)
        binary.unlink()
    run=subprocess.run([sys.executable,'-m','unittest','discover','-s',str(HERE/'tests'),'-p','test_wifi*.py'],
                       cwd=ROOT,capture_output=True,text=True,check=True,timeout=90)
    checks['python']=run.stdout+run.stderr;(out/'python.log').write_text(checks['python'])
    paths=[HERE/'boot.py',HERE/'wifi_check.py',HERE/'wifi_payload.py',HERE/'riplix/root.mjs',
           HERE/'boot/net.c',HERE/'boot/net.h',HERE/'boot/network.inc',
           *sorted((HERE/'boot').glob('wifi*')),*sorted((HERE/'tests').glob('wifi*')),
           *sorted((HERE/'tests').glob('test_wifi*')),*sorted((HERE/'tests/fixtures').glob('wifi*'))]
    hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths if p.is_file()}
    report.write_text(json.dumps(dict(status='passed',sanitizer='undefined,fatal',checks=checks,source_sha256=hashes,
                                     hardware='No RF emulation; physical live NIC/IP still requires HP acceptance'),indent=2)+'\n')
    print(f'PASS: {len(SUITES)} Wi-Fi suites, virtio/mailbox guards and Python profile/packaging checks; no hardware claims')


if __name__=='__main__':main()
