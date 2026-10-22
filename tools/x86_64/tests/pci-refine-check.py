#!/usr/bin/env python3
"""Focused early BAR refinement mock; no device or HP access."""
import hashlib,json,pathlib,subprocess,sys
root=pathlib.Path(__file__).resolve().parents[3]
out=root/'build/x86_64-touchpad';out.mkdir(parents=True,exist_ok=True)
flags=['clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all']
sdk=pathlib.Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk')
if sys.platform=='darwin' and sdk.is_dir():flags+=['-isysroot',str(sdk)]
exe=out/'pci-refine-ubsan'
subprocess.run(flags+['tools/x86_64/tests/pci-refine.c','-o',str(exe)],cwd=root,check=True,timeout=30)
p=subprocess.run([str(exe)],cwd=root,check=True,text=True,capture_output=True,timeout=10)
files=['tools/x86_64/boot/pci-refine.c','tools/x86_64/boot/pci-refine.h','tools/x86_64/tests/pci-refine.c','tools/x86_64/tests/pci-refine-check.py','tools/x86_64/boot/bootinfo.h']
result={'passed':True,'physical_acceptance':False,'output':p.stdout.strip(),
 'sha256':{n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in files}}
(out/'pci-refine-result.json').write_text(json.dumps(result,indent=2)+'\n')
print(p.stdout,end='')
