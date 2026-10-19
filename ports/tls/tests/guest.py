"""Disposable real kernels; all network traffic travels through guest Ethernet."""
from pathlib import Path
import fcntl
import struct
import termios
import json
import os
import pty
import select
import subprocess
import time
import tty
ROOT = Path(__file__).resolve().parents[3]


class Guest:
    def __init__(self, directory, target, applications, files=(), peer=None, blocks=32768, reuse=False,
                 disk_sectors=393216):
        self.directory = Path(directory)
        self.target, self.peer = target, peer
        native = target == 'native'
        build = ROOT / ('fuse-x/build' if native else 'build/fuzix')
        self.image = self.directory / 'disk.img'
        manifest = []
        for name in ('sh', 'echo', 'cat'):
            manifest.append(dict(name='/bin/' + name, mode=0o100755,
                path=str(build / ('apps/' + name + '.fxe' if native else name + '.rxe'))))
        for name, path in applications.items():
            manifest.append(dict(name='/bin/' + name, mode=0o100755, path=str(Path(path).resolve())))
        manifest += list(files)
        builder = r'''
import fs from 'node:fs';
import {rootImage} from './ports/fuzix/tools/disk.mjs';
import {Image,writeImage} from './ports/fuzix/tools/image.mjs';
import {fat16Image} from './c-test/examples/rOS/mkfat16.mjs';
const [manifest,disk,build,native,blocks,sectors]=process.argv.slice(1);
const files=JSON.parse(manifest).map(f=>({...f,data:f.path?fs.readFileSync(f.path):Buffer.from(f.text||'')}));
files.push({name:'/init',mode:0o100755,data:native==='1'?Buffer.from('FXN1:init:1\n'):fs.readFileSync(build+'/init.rxe')},
{name:'/dev/tty',mode:0o20666,device:0x200},{name:'/dev/console',mode:0o20600,device:0x201},
{name:'/dev/null',mode:0o20666,device:0x100},{name:'/dev/urandom',mode:0o20444,device:0x104},
{name:'/dev/crypto',mode:0o20666,device:0x105,data:Buffer.alloc(0)},
{name:'/etc/resolv.conf',data:Buffer.from('nameserver 192.168.64.1\n')});
const root=rootImage(files,Number(blocks),256);
if(native==='1')writeImage(disk,root);else writeImage(disk,fat16Image([
{name:'FUZIX.SYS',data:fs.readFileSync(build+'/FUZIX.SYS')},{name:'ROOTFS.IMG',data:root}],
{totalSectors:Number(sectors),sectorsPerFat:256,sectorsPerCluster:8,imageFactory:size=>new Image(size)}));
'''
        if not reuse:
            subprocess.run(['node','--input-type=module','-e',builder,json.dumps(manifest),
                            str(self.image),str(build),'1' if native else '0',str(blocks),str(disk_sectors)],cwd=ROOT,check=True)
        network = str(self.directory / 'nic') if peer else 'off'
        if native:
            command=[str(build/'fuse-x'),'--run',str(self.image)]
        else:
            command=[os.environ.get('RIPPLE_TEST_RVM',str(ROOT/'target/release/rvm')),str(build/'bios.bin'),'--disk',str(self.image),
                     '--memory','16384000','--jit','--guest-ctrl-c','--net',network,'--timeout','900']
        self.master,slave=pty.openpty();tty.setraw(slave)
        fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack("HHHH",24,80,0,0))
        self.process=subprocess.Popen(command,stdin=slave,stdout=slave,stderr=slave,
            env=dict(os.environ,TERM='xterm',RCC_NATIVE_HEADLESS='1',RCC_NATIVE_NET=network,RCC_NATIVE_TIMEOUT='900'),
            start_new_session=True)
        os.close(slave)
        self.log=bytearray();self.at=0
        self.prompt=b'fuse-x# ' if native else b'fuzix# '
        (self.directory/'vm.pid').write_text(str(self.process.pid))
        try:
            self.read(self.prompt)
        except BaseException:
            self.process.kill();self.process.wait();os.close(self.master)
            raise

    def pump(self):
        if self.peer:self.peer.pump()
        if select.select([self.master],[],[],.005)[0]:
            try:self.log.extend(os.read(self.master,65536))
            except OSError:pass
        (self.directory/'guest.log').write_bytes(self.log)

    def read(self,marker,seconds=180):
        until=time.monotonic()+seconds
        while marker not in self.log[self.at:]:
            self.pump()
            if marker in self.log[self.at:]:break
            if self.process.poll() is not None or time.monotonic()>until:
                raise AssertionError(f'Guest did not produce {marker!r}\n'+self.log[-6000:].decode(errors='replace'))
        end=self.log.index(marker,self.at)+len(marker)
        text=bytes(self.log[self.at:end]).replace(b'\r',b'').decode(errors='replace')
        self.at=end
        return text

    def shell(self,command,seconds=180):
        os.write(self.master,command.encode()+b'\r')
        return self.read(self.prompt,seconds)

    def close(self):
        try:
            if self.process.poll() is None:
                os.write(self.master,b'shutdown\r')
                self.read(b'filesystem synced; session stopped' if self.target=='native' else b'power off.',60)
                self.process.wait(timeout=10)
        finally:
            if self.process.poll() is None:self.process.kill();self.process.wait()
            os.close(self.master)
        if self.target=='native':
            import sys
            sys.path.insert(0,str(ROOT/'fuse-x'))
            from test_support import check_root
            check_root(self.image)
        else:subprocess.run(['python3',str(ROOT/'ports/fuzix/tools/fsck.py'),str(self.image)],check=True)
