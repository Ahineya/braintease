#!/usr/bin/env python3
"""Focused fake UEFI PCI-resource capture; no firmware or device access."""
import hashlib,json,pathlib,re,subprocess,sys
root=pathlib.Path(__file__).resolve().parents[3]
out=root/'build/x86_64-pci-uefi';out.mkdir(parents=True,exist_ok=True)
flags=['clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all']
sdk=pathlib.Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk')
if sys.platform=='darwin' and sdk.is_dir():flags+=['-isysroot',str(sdk)]
source='tools/x86_64/tests/pci-uefi.c';exe=out/'pci-uefi-ubsan'
subprocess.run(flags+[source,'-o',str(exe)],cwd=root,check=True,timeout=30)
p=subprocess.run([str(exe)],cwd=root,check=True,text=True,capture_output=True,timeout=10)
match=re.search(r'PCI_UEFI_CAPTURE_OK groups=(\d+)',p.stdout);assert match,p.stdout
files=[source,'tools/x86_64/tests/pci-uefi-check.py','tools/x86_64/boot/pci-uefi.c',
       'tools/x86_64/boot/pci-uefi.h','tools/x86_64/boot/bootinfo.h','tools/x86_64/boot/uefi.h']
result={'status':'passed','scope':'focused fake UEFI protocol capture/parser; no firmware/physical acceptance',
 'groups':int(match.group(1)),'sanitizer':'undefined, fatal','physical_acceptance':False,
 'borrowed_root_descriptor_not_freed':True,'caller_owned_buffers_freed':True,'PCI_writes_or_BAR_probes':False,
 'BAR_AddrRangeMax_semantics':'EDK2 alignment mask / auxiliary; mapped extent from checked Min+Len',
 'root_AddrRangeMax_semantics':'inclusive ending address remains strictly validated',
 'raw_descriptor_failure_diagnostic':'persistent reason[96]: ROOT/BAR type Min Max Len; no BootInfo size change',
 'primary_source':'https://github.com/tianocore/edk2/blob/master/MdeModulePkg/Bus/Pci/PciBusDxe/PciIo.c',
 'command':'python3 tools/x86_64/tests/pci-uefi-check.py','output':p.stdout.strip(),
 'source_sha256':{n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in files}}
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print(p.stdout,end='')
