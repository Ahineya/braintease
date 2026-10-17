#!/usr/bin/env python3
"""JIT checks for V3 allocation above 128 MiB, sparse trees and wide truncation."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from launch import require_mmu
from root_reader import RootReader
ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
a = p.parse_args()
build = a.build.resolve()
require_mmu(build)
libs = subprocess.check_output(['make','-s','--no-print-directory','-C',str(PORT),f'BUILD={build}',
    '-f','Makefile','-f','-','largefs-libs'],input='largefs-libs:\n\t@echo $(LIBC_OBJS)\n',text=True).split()
with tempfile.TemporaryDirectory(prefix='fuzix-largefs-') as directory:
    tmp = Path(directory)
    asm,obj,exe = [tmp / ('largefscheck' + suffix) for suffix in ('.asm','.pobj','.rxe')]
    subprocess.run([str(ROOT/'target/release/rcc'),'compile',str(PORT/'tools/largefscheck.c'),'-o',str(asm),
        '--bank-size','64000','-DRIPPLE_MMU=1','-DNO_64BIT',
        *[f'-I{v}' for v in (PORT/'ripple/include',PORT/'upstream/Library/include',ROOT/'runtime/include',PORT/'ripple')]],check=True)
    subprocess.run([str(ROOT/'src/ripple-asm/target/release/rasm'),'assemble',str(asm),'-o',str(obj),
                    '--bank-size','64000','--max-immediate','65535'],check=True)
    subprocess.run([str(ROOT/'src/ripple-asm/target/release/rlink'),str(build/'user/entry.pobj'),str(obj),
                    str(build/'user/userlib.pobj'),*libs,'-f','rxe','--bank-size','64000','-o',str(exe)],check=True)
    for file in build.iterdir():
        if file.suffix=='.rxe' or file.name in ('FUZIX.SYS','bios.bin'):
            (tmp/file.name).symlink_to(file)
    (tmp/'apps.json').write_text(json.dumps([*json.loads((build/'apps.json').read_text()),'largefscheck']))
    image=tmp/'disk.img'
    subprocess.run(['node',str(PORT/'tools/disk.mjs'),str(tmp),str(image)],check=True)
    reader=RootReader(image)
    initial=reader.wide(1024+160)
    reader.disk.close()
    for mode in ('create','verify','cleanup'):
        result=subprocess.run([str(ROOT/'target/release/rvm'),str(build/'bios.bin'),'--disk',str(image),
            '--memory','16384000','--jit','--headless','--input',f'largefscheck {mode}\necho LARGEFS:$?\nshutdown\n',
            '--timeout','240'],capture_output=True,text=True,timeout=250)
        output=result.stdout+result.stderr
        Path(f'/tmp/fuzix-largefs-{mode}.log').write_text(output)
        assert result.returncode==0 and '\nLARGEFS:0\n' in output and 'disk synced; power off.' in output,output
        assert 'FAILED' not in output and 'panic:' not in output,output
        subprocess.run(['python3',str(PORT/'tools/fsck.py'),str(image)],check=True)
        reader=RootReader(image)
        if mode=='create':
            ino=reader.lookup('/wide-dense')
            assert reader.block(ino,69999)>65535, 'must allocate physical blocks above the former ceiling'
            assert reader.wide(reader.base(reader.lookup('/wide-sparse'))+4)>128*1024*1024
        if mode=='cleanup':
            assert reader.wide(1024+160)==initial,'lost blocks after wide tree truncation/unlink'
        reader.disk.close()
        print(f'JIT C16 V3 {mode} passed',flush=True)
    # A 1,001 MiB root with 16 KiB FAT clusters needs 64,064 entries: its
    # final cluster lives in the second supervisor bank of the chain cache.
    subprocess.run(['node','--input-type=module','-e',r'''
import {openImage,Image} from './ports/fuzix/tools/image.mjs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
import {rootReader} from './ports/fuzix/tools/read-root.mjs';
import {checkRoot} from './ports/fuzix/tools/fsck.mjs';
import {atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {fat16Image} from './c-test/examples/rOS/mkfat16.mjs';
const name=process.argv[1],disk=openImage(name);
let root=installRoot(fatFile(disk,'ROOTFS.IMG').data,[{name:'/bank-end',data:Buffer.from('BANKMAP_HIGH_OK\n')}]);
const node=rootReader(root).lookup('/bank-end'),old=node.map.get(0);
root=root.fork(1001*1048576);root.writeUInt32LE(root.length/2048,(1024+2)*2);
const high=root.length/2048-1;root.copy(root,high*2048,old*2048,(old+1)*2048);
root.writeUInt32LE(high,(node.base+12)*2);root=checkRoot(root,true).image;
atomicWrite(name,fat16Image([{name:'FUZIX.SYS',data:fatFile(disk,'FUZIX.SYS').data},{name:'ROOTFS.IMG',data:root}],
  {totalSectors:1023*2048,sectorsPerFat:256,sectorsPerCluster:32,imageFactory:n=>new Image(n)}));
''',str(image)],cwd=ROOT,check=True)
    result=subprocess.run([str(ROOT/'target/release/rvm'),str(build/'bios.bin'),'--disk',str(image),
        '--memory','16384000','--jit','--headless','--input','cat /bank-end\necho BANKMAP:$?\nshutdown\n',
        '--timeout','120'],capture_output=True,text=True,timeout=130)
    output=result.stdout+result.stderr
    Path('/tmp/fuzix-largefs-bank-map.log').write_text(output)
    assert result.returncode==0 and '\nBANKMAP_HIGH_OK\n' in output and '\nBANKMAP:0\n' in output,output
    assert 'disk synced; power off.' in output,output
    subprocess.run(['python3',str(PORT/'tools/fsck.py'),str(image)],check=True)
    print('JIT C16 V3 FAT chain above 64,000 entries passed',flush=True)
