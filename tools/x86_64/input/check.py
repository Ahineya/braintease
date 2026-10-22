#!/usr/bin/env python3
"""Small host fixtures for the native I2C/HID boundary; no EFI/guest rebuild."""
import hashlib
import json
import pathlib
import subprocess
import sys
import time

repo=pathlib.Path(__file__).resolve().parents[3]
out=repo/'build/x86_64-touchpad'
out.mkdir(parents=True,exist_ok=True)
compiler=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
          '-fsanitize=undefined','-fno-sanitize-recover=all']
old_sdk=pathlib.Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk')
if sys.platform=='darwin' and old_sdk.is_dir():compiler+=['-isysroot',str(old_sdk)]
checks={
 'pci-resource':['tools/x86_64/tests/pci-resource.c','tools/x86_64/boot/pci-resource.c'],
 'i2c-hp':['tools/x86_64/tests/i2c-hp.c','tools/x86_64/boot/i2c-hp.c'],
 'i2c-designware':['tools/x86_64/tests/i2c-designware.c','tools/x86_64/boot/i2c-designware.c'],
 'hid-i2c-pointer':['tools/x86_64/tests/hid-i2c-pointer.c','tools/x86_64/boot/hid_i2c.c']}
result={'schema':1,'physical_acceptance':False,'checks':{}}
for name,files in checks.items():
 target=out/(name+'-ubsan')
 subprocess.run(compiler+files+['-o',str(target)],cwd=repo,check=True,timeout=30)
 run=subprocess.run([str(target)],cwd=repo,check=True,text=True,capture_output=True,timeout=15)
 result['checks'][name]={'passed':True,'output':run.stdout.strip()}
subprocess.run(compiler+['-fsyntax-only','tools/x86_64/boot/i2c-hp.c'],cwd=repo,check=True,timeout=15)
files=[p for pattern in ('tools/x86_64/boot/i2c-*.c','tools/x86_64/boot/i2c-*.h',
 'tools/x86_64/boot/hid_i2c.*','tools/x86_64/boot/pci-resource.*','tools/x86_64/tests/pci-resource.c','tools/x86_64/tests/i2c-*.c','tools/x86_64/tests/hid-i2c-*.c',
 'tools/x86_64/tests/fixtures/hid-i2c-*.bin') for p in repo.glob(pattern)]
result['sha256']={str(p.relative_to(repo)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(files)}
result['timestamp']=int(time.time())
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result['checks'],indent=2))
