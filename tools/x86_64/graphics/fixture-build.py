#!/usr/bin/env python3
"""Build a disposable virtio-root GUI fixture from frozen C16 kernel IR output."""
from pathlib import Path
import hashlib,json,os,shutil,subprocess,sys
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools/x86_64'))
from boot import elf_linker,linker,elf_symbols
from usb_image import boot_disk
os.umask(0o077)
base=ROOT/'build/x86_64-network/test';d=ROOT/'build/x86_64-graphics/fixture';d.mkdir(parents=True,exist_ok=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
common=['-ffreestanding','-fno-builtin','-fno-stack-protector','-mno-red-zone','-mgeneral-regs-only','-O2','-Wall','-Wextra','-Werror','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-ffunction-sections','-fdata-sections']
defines=['-DBOOT_TEST=1','-DRIPLIX_BOOT=1','-DPERSISTENT_ROOT=1','-DBOOT_NETWORK=1','-DBOOT_MATH=1','-DBOOT_GRAPHICS=1','-DBOOT_FONT_ATLAS=1','-DCONSOLE_SCALE=1','-DBOOT_FAULT=0']
objects=[base/'rcc.o',base/'font.o',ROOT/'build/x86_64-terminal-wave32/normal/math.o']
for name in ('arch.S','kernel.c','console.c','input.c','keyboard.c','timer.c','entropy.c','acpi.c','power.c','runtime.c','pci.c','dma.c','net.c','block.c','gx.c'):
    source=ROOT/'tools/x86_64/boot'/name;obj=d/(source.stem+'.o')
    if '--gx-only' not in sys.argv or name=='gx.c':
        subprocess.run(['clang','--target=x86_64-unknown-none-elf',*common,'-fpie',*defines,'-c',str(source),'-o',str(obj)],check=True)
    objects.append(obj)
objects += [base/'vectors.o',base/'irq.o']
definitions={'kernel_readonly_end'};imports=set()
for obj in objects:
    a,b=elf_symbols(obj);definitions.update(a);imports.update(b)
missing=sorted(imports-definitions)
if any(not x.startswith('rcc_native_') for x in missing):raise RuntimeError('unbound platform symbols: '+str(missing))
asm=['.text']
for i,name in enumerate(missing):asm += [f'.globl {name}',f'{name}:',f'lea unsupported_{i}(%rip),%rdi','jmp unsupported_service']
asm+=['.section .rodata']+[f'unsupported_{i}: .asciz "unsupported service: {name}"' for i,name in enumerate(missing)]+['.section .note.GNU-stack,"",@progbits']
(d/'unsupported.S').write_text('\n'.join(asm)+'\n')
subprocess.run(['clang','--target=x86_64-unknown-none-elf','-c',str(d/'unsupported.S'),'-o',str(d/'unsupported.o')],check=True);objects.append(d/'unsupported.o')
subprocess.run([*elf_linker(),'-pie','--no-dynamic-linker','-Bsymbolic','--no-undefined','--gc-sections','-T',str(ROOT/'tools/x86_64/boot/kernel.ld'),'-o',str(d/'kernel.elf'),*map(str,objects)],check=True)
(d/'payload.S').write_text('.section .rdata,"dr"\n.globl kernel_payload_start,kernel_payload_end\nkernel_payload_start:\n.incbin "'+str(d/'kernel.elf')+'"\nkernel_payload_end:\n')
subprocess.run(['clang','--target=x86_64-pc-windows-msvc','-c',str(d/'payload.S'),'-o',str(d/'payload.obj')],check=True)
efi=d/'esp/EFI/BOOT/BOOTX64.EFI';efi.parent.mkdir(parents=True,exist_ok=True)
subprocess.run([*linker(),'/subsystem:efi_application','/entry:efi_main','/nodefaultlib','/machine:x64','/dynamicbase','/base:0x100000','/opt:ref','/timestamp:0',f'/out:{efi}',str(base/'loader.obj'),str(d/'payload.obj')],check=True)
image=boot_disk(d/'esp')
root=d/'root.img'
if not root.exists():subprocess.run(['node',str(Path(__file__).with_name('fixture-root.mjs')),str(base/'root.img'),str(root),str(d.parent/'apps')],check=True)
for name in ('edk2-x86_64-code.fd','edk2-i386-vars.fd'):shutil.copyfile(Path('/opt/homebrew/share/qemu')/name,d/name)
(d/'fixture.json').write_text(json.dumps({'frozen_kernel':{'path':str(base/'rcc.o'),'sha256':sha(base/'rcc.o')},'efi_sha256':sha(efi),'image':str(image),'root_bytes':root.stat().st_size,'unsupported':missing},indent=2)+'\n')
print('Focused fixture ready',d,flush=True)
