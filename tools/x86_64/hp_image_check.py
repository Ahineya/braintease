#!/usr/bin/env python3
"""Exact HP EFI acceptance with fake payloads and captured GPT geometry only.
Never opens a physical disk. The virtual Ubuntu/ESP contents are canaries.
"""
import argparse,hashlib,json,shutil,struct,sys,tempfile,uuid
from pathlib import Path
from types import SimpleNamespace
from boot import executable,firmware,validate_root_image
from gpt_image import load_capture,write_image
from qemu_check import run_shell_check
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,required=True)
p.add_argument('--capture',type=Path,required=True)
p.add_argument('--identity',type=Path,required=True)
p.add_argument('--accel',choices=['tcg','kvm'],default='tcg')
p.add_argument('--timeout',type=int,default=600)
p.add_argument('--wifi-control',action='store_true',help='exercise installed x64 radio control against absent device')
p.add_argument('--reboot-second',action='store_true',help='finish second boot through actual firmware reset, with QEMU no-reboot')
p.add_argument('--console-input',action='store_true',help='verify Escape/navigation/function-key octets through i8042/TTY/shell')
a=p.parse_args();w=a.build.resolve();o=w/'hp-production-check';o.mkdir(exist_ok=True)
(o/'result.json').unlink(missing_ok=True)
ident=json.loads(a.identity.read_text());capture=load_capture(a.capture)
# Reject stale historical captures before launching a misleading boot test.
assert capture['sector']==ident['sector_bytes'],'capture/identity sector mismatch'
header=capture['spans'][capture['sector']]
assert str(uuid.UUID(bytes_le=bytes(header[56:72])))==ident['disk_guid'],'capture/identity disk GUID mismatch'
array_lba,entries,entry_size=struct.unpack_from('<QII',header,72)
assert 0<entries<=128 and 128<=entry_size<=512,'unsupported capture entry geometry'
array=capture['spans'][array_lba*capture['sector']]
matches=[array[i*entry_size:(i+1)*entry_size] for i in range(entries)
         if array[i*entry_size+16:i*entry_size+32]==uuid.UUID(ident['partition_guid']).bytes_le]
assert len(matches)==1,'capture lacks unique selected Riplix partition; use its current GPT capture'
assert struct.unpack_from('<QQ',matches[0],32)==(ident['first_lba'],ident['last_lba']),'capture/identity partition bounds mismatch'
seed=w/'normal/root.img';validate_root_image(seed);size=seed.stat().st_size
first=ident['first_lba']*ident['sector_bytes'];last=(ident['last_lba']+1)*ident['sector_bytes']
assert size==1024**3 and size<=last-first
regions=[(int(at),len(raw)) for at,raw in capture['spans'].items()]
for at in [1024**2,2203648*512,500*1024**3,first-4096,first+size,last-4096,last]:
 assert all(not(at<b+n and b<at+4096) for b,n in regions)
 capture['spans'][at]=bytes((i*17+at//4096)%251 for i in range(4096));regions.append((at,4096))
image=w/'normal/usb-boot.img';image_hash=hashlib.file_digest(image.open('rb'),'sha256').hexdigest()
with tempfile.TemporaryDirectory(prefix='hp-fixture-',dir=o) as temporary:
 d=Path(temporary);root=d/'disk.img';write_image(root,capture)
 with seed.open('rb') as source,root.open('r+b') as target:
  at=first
  while chunk:=source.read(1024**2):
   if any(chunk):target.seek(at);target.write(chunk)
   at+=len(chunk)
 def fingerprint():
  h=hashlib.sha256()
  with root.open('rb') as f:
   for at,n in sorted(regions):f.seek(at);raw=f.read(n);assert len(raw)==n;h.update(raw)
  return {'coverage':'both GPT copies and seven named4KiB canaries; NOT whole outside disk','ranges':sorted(regions),'sha256':h.hexdigest()}
 before=fingerprint();code,variables=firmware(SimpleNamespace(firmware_code=None,firmware_vars=None))
 var=d/'vars.fd';shutil.copyfile(variables,var);serial=o/'serial.log'
 command=[executable('qemu-system-x86_64'),'-machine','q35','-accel',a.accel,'-cpu','host' if a.accel=='kvm' else 'max','-m','1024M','-smp','1','-no-reboot','-nic','none','-monitor','none','-display','none','-drive',f'if=pflash,format=raw,readonly=on,file={code}','-drive',f'if=pflash,format=raw,file={var}','-serial',f'file:{serial}','-device','qemu-xhci','-drive',f'if=none,id=bootusb,format=raw,snapshot=on,file={image}','-device','usb-storage,drive=bootusb,removable=on','-blockdev',json.dumps({'driver':'raw','node-name':'riplix-root','file':{'driver':'file','filename':str(root)},'cache':{'direct':False,'no-flush':False}}),'-device','nvme,drive=riplix-root,serial=RIPLIXFIXTURE,logical_block_size=512,physical_block_size=512']
 with (o/'qemu.log').open('w') as log:
  for i in range(2):
   serial.unlink(missing_ok=True)
   extra=[('entropycheck',r'entropycheck passed'),('sh /offline-check.sh',r'OFFLINE_APPS_OK')]
   if a.wifi_control:
    extra += [('wifi status',r'WiFi unavailable'),('wifi down',r'WiFi unavailable'),('wifi reconnect; echo CONTROL_STATUS:$?',r'AX211 is unavailable[\s\S]*CONTROL_STATUS:1')]
   reboot=a.reboot_second and i==1
   result=run_shell_check(command,serial,log,a.timeout,o/f'screen-{i+1}.png',debug_exit=False,persistent=True,restart=bool(i),storage='nvme',system_services=True,existing_root=True,extra_checks=extra,exit_command='reboot' if reboot else 'shutdown',console_input_check=a.console_input)
   assert result.returncode==0
   text=serial.read_text();(o/f'boot-{i+1}.log').write_text(text)
   for marker in ['ROOT_PARTITION_OK','ROOT_STORAGE nvme persistent','WIFI_BRINGUP_SKIPPED no_ax211','PLATFORM_REBOOT' if reboot else 'RIPLIX_X86_64_BOOT_OK','OFFLINE_APPS_OK','filesystem synced; session stopped']:assert marker in text,marker
   assert 'BOOT_FAIL' not in text and 'EXCEPTION' not in text
   assert fingerprint()==before,'metadata/canary changed'
 result={'status':'passed','accelerator':a.accel,'production':True,'persistence_boots':2,'firmware_reboot_second_boot':a.reboot_second,'console_input_octets':a.console_input,'root_identity':ident,'root_bytes':size,'boot_image_sha256':image_hash,'efi_sha256':hashlib.file_digest((w/'normal/esp/EFI/BOOT/BOOTX64.EFI').open('rb'),'sha256').hexdigest(),'outside_before':before,'outside_after':fingerprint(),'wifi':'absent-radio failure path only; RF untested','physical_native_acceptance':False}
 assert hashlib.file_digest(image.open('rb'),'sha256').hexdigest()==image_hash
 (o/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2),flush=True)
