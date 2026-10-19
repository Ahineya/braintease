#!/usr/bin/env python3
"""Select readiness against actual JIT/native kernels, private disks and Ethernet peer."""
import argparse
import json
import os
import pty
from pathlib import Path
import select
import struct
import subprocess
import sys
import tempfile
import time
from network_peer import Peer, Stream, wire
ROOT=Path(__file__).resolve().parents[3]
PORT=ROOT/'ports/fuzix'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
p.add_argument('--native',action='store_true')
p.add_argument('--local',action='store_true',help='exercise Unix sockets offline instead of internet readiness')
p.add_argument('--capacity-only',action='store_true',help='run only listener/socket capacity and readiness checks')
p.add_argument('--memory-stress',action='store_true',help='exhaust the RVM page pool, then check networking still works')
p.add_argument('--native-build',type=Path,default=ROOT/'fuse-x/build')
a=p.parse_args()
if a.memory_stress and (a.native or a.local):p.error('--memory-stress requires RVM internet tests')
probe='localcheck' if a.local else 'selectcheck';build=a.build.resolve();native=ROOT/'fuse-x';native_build=a.native_build.resolve()

class SelectPeer(Peer):
    def packet(self,frame):
        if len(frame)>=54 and frame[12:14]==b'\x08\x00' and frame[23]==6:
            at=14+(frame[14]&15)*4
            src,dst,seq,ack,off,flags,window,checksum,urgent=struct.unpack('!HHIIBBHHH',frame[at:at+20])
            if dst==7002 and flags&2:
                # Independent refusal; no guest stack code generates this RST.
                s=object.__new__(Stream);s.owner=self;s.local=dst;s.remote=src;s.peer_next=(seq+1)&0xffffffff
                self.send(s.frame(0,20));return
        super().packet(frame)

with tempfile.TemporaryDirectory(prefix='fselect-',dir='/tmp') as temp:
    tmp=Path(temp);exe=tmp/(probe+('.fxe' if a.native else '.rxe'))
    if a.native:
        subprocess.run([sys.executable,str(native/'cc.py'),str(PORT/('tools/'+probe+'.c')),
                        '--build',str(native_build),'-o',str(exe)],check=True)
        image=tmp/'root.img'
        subprocess.run(['node',str(native/'image.mjs'),'create',str(image),'--apps',str(native_build/'apps')],check=True)
        subprocess.run(['node',str(native/'image.mjs'),'put',str(image),'/bin/'+probe,str(exe)],check=True)
    else:
        libs=subprocess.check_output(['make','-s','--no-print-directory','-C',str(PORT),f'BUILD={build}',
            '-f','Makefile','-f','-','select-libs'],input='select-libs:\n\t@echo $(LIBC_OBJS)\n',text=True).split()
        asm=tmp/'check.asm';obj=tmp/'check.pobj'
        subprocess.run([str(ROOT/'target/release/rcc'),'compile',str(PORT/('tools/'+probe+'.c')),'-o',str(asm),
            '--bank-size','64000','-DRIPPLE_MMU=1','-DNO_64BIT',*[f'-I{x}' for x in
            (PORT/'ripple/include',PORT/'upstream/Library/include',ROOT/'runtime/include',PORT/'ripple')]],check=True)
        subprocess.run([str(ROOT/'src/ripple-asm/target/release/rasm'),'assemble',str(asm),'-o',str(obj),
                        '--bank-size','64000','--max-immediate','65535'],check=True)
        subprocess.run([str(ROOT/'src/ripple-asm/target/release/rlink'),str(build/'user/entry.pobj'),str(obj),
            str(build/'user/userlib.pobj'),*libs,'-f','rxe','--bank-size','64000','-o',str(exe)],check=True)
        stage=tmp/'stage';stage.mkdir()
        names=['init','sh','echo','cat','true','ifconfig']
        if a.memory_stress:names+=['hello','mmucheck','ping']
        for name in [*(n+'.rxe' for n in names),'FUZIX.SYS','bios.bin']:(stage/name).symlink_to(build/name)
        (stage/(probe+'.rxe')).symlink_to(exe);(stage/'apps.json').write_text(json.dumps([*names,probe]))
        image=tmp/'disk.img';subprocess.run(['node',str(PORT/'tools/disk.mjs'),str(stage),str(image)],check=True)
    peer=None if a.local else SelectPeer(tmp/'nic');env=dict(os.environ)
    def pump():
        if peer:peer.pump()
        else:time.sleep(0.01)
    if a.native:
        env.pop('RIPPLE_NET',None)
        env['RCC_NATIVE_NET']='off'
        env.update(RCC_NATIVE_HEADLESS='1',RCC_NATIVE_TIMEOUT='300')
        if peer:env['RCC_NATIVE_NET']=str(tmp/'nic')
        cmd=[str(native_build/'fuse-x'),'--run',str(image)]
    else:
        cmd=[str(ROOT/'target/release/rvm'),str(build/'bios.bin'),'--disk',str(image),'--memory','16384000',
             *(['--net',str(tmp/'nic')] if peer else []),'--jit','--timeout',str(900 if a.memory_stress else 300)]
    log=Path('/tmp/fuzix-'+('local-' if a.local else 'select-')+('native' if a.native else 'jit')+'.log')
    with log.open('wb') as output:
        master,slave=pty.openpty() if not a.native else (None,None)
        guest=subprocess.Popen(cmd,stdin=subprocess.PIPE if a.native else slave,stdout=output,stderr=subprocess.STDOUT,env=env)
        if slave is not None:os.close(slave)
        def console():return log.read_bytes().replace(b'\r',b'')
        def until(marker,seconds=60):
            end=time.monotonic()+seconds
            while marker not in console():
                assert guest.poll() is None and time.monotonic()<end,(marker,console()[-6000:],
                    [(s.local,s.remote,s.established,s.fin,s.fin_acked,len(s.received),len(s.pending),s.window) for s in peer.streams.values()] if peer else [])
                assert b'FAILED' not in console() and b'panic:' not in console(),console()[-6000:]
                pump()
        def send(line):
            if a.native:guest.stdin.write(line.encode()+b'\n');guest.stdin.flush()
            else:os.write(master,line.encode()+b'\r')
        try:
            until(b'fuse-x# ' if a.native else b'riplix# ')
            if a.local:
                send('localcheck; echo LOCAL_STATUS:$?')
                until(b'LOCAL_STATUS:0\n',90);assert b'LOCAL_OK' in console(),console()
            else:
                if not a.capacity_only:
                    send('selectcheck; echo SELECT_STATUS:$?')
                    until(b'SELECT_STATUS:0\n',90);assert b'SELECT_OK' in console(),console()
                    send('selectcheck input; echo INPUT_STATUS:$?');until(b'SELECT_INPUT_READY\n')
                    send('k');until(b'INPUT_STATUS:0\n');assert b'SELECT_INPUT_OK' in console()
                    # A blocked selector should leave the VM/host idle, rather than
                    # repeatedly executing a runnable application polling loop.
                    send('selectcheck idle; echo IDLE_STATUS:$?');until(b'SELECT_IDLE_READY\n')
                    cpu_before=subprocess.check_output(['ps','-o','time=','-p',str(guest.pid)],text=True).strip()
                    until(b'IDLE_STATUS:0\n');cpu_after=subprocess.check_output(['ps','-o','time=','-p',str(guest.pid)],text=True).strip()
                    def cpu(value):
                        return sum(float(field)*60**position for position,field in enumerate(reversed(value.split(':'))))
                    spent=cpu(cpu_after)-cpu(cpu_before)
                    assert spent<1.5,(cpu_before,cpu_after)
                    print(f'Select idle: {spent:.3f}s host CPU during a two-second guest wait',flush=True)
                send('selectcheck '+('capacity-stress' if a.memory_stress else 'capacity')+'; echo CAPACITY_STATUS:$?')
                until(b'SELECT_CAPACITY_LISTENERS_READY\n',90)
                listeners=[]
                for index in range(16):
                    client=peer.connect(7400+index,45000+index)
                    client.queue(bytes([index]));client.closing=True
                    listeners.append(client)
                until(b'CAPACITY_STATUS:0\n',600 if a.memory_stress else 180);assert b'SELECT_CAPACITY_OK' in console(),console()
                assert all(bytes(client.received)==bytes([index]) for index,client in enumerate(listeners))
                if a.memory_stress:
                    assert b'CAPACITY_MEMORY_PRESSURE_OK' in console(),console()
                    print('MMU exhaustion/recovery preserved data queued on all 64 TCP streams',flush=True)
                send('selectcheck network; echo NETWORK_STATUS:$?');until(b'SELECT_LISTEN_READY\n')
                stream=peer.connect(7331,43331)
                end=time.monotonic()+10
                while not stream.established:
                    assert time.monotonic()<end,console();pump()
                stream.queue(b'L')
                end=time.monotonic()+10
                while stream.pending or stream.flight:
                    assert time.monotonic()<end,console();pump()
                # Receiving a transport ACK does not prove the app consumed L.
                until(b'SELECT_RESET_READY\n')
                stream.send(stream.next,20)
                until(b'NETWORK_STATUS:0\n');assert b'SELECT_NETWORK_OK' in console()
            send('shutdown')
            if a.native:guest.stdin.close()
            end=time.monotonic()+20
            while guest.poll() is None:
                assert time.monotonic()<end,console();pump()
            assert guest.returncode==0,console()
        finally:
            if guest.poll() is None:guest.kill();guest.wait()
            if peer:peer.close()
            if master is not None:os.close(master)
    if a.native:
        sys.path.insert(0,str(native));from test_support import check_root
        check_root(image)
    else:subprocess.run([sys.executable,str(PORT/'tools/fsck.py'),str(image)],check=True)
    if a.local:
        # Keep the stale socket node through an install and a real second boot.
        if a.native:
            subprocess.run(['node',str(native/'image.mjs'),'install',str(image),'--apps',str(native_build/'apps')],check=True)
            reboot=subprocess.run(cmd,input=b'localcheck stale; echo STALE_STATUS:$?\nshutdown\n',
                                  stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=env,timeout=60)
        else:
            reboot=subprocess.run([*cmd,'--headless','--input','localcheck stale; echo STALE_STATUS:$?\nshutdown\n'],
                                  stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env=env,timeout=60)
        output=reboot.stdout.replace(b'\r',b'')
        assert reboot.returncode==0 and b'LOCAL_STALE_OK' in output and b'STALE_STATUS:0\n' in output,output
        if a.native:check_root(image)
        else:subprocess.run([sys.executable,str(PORT/'tools/fsck.py'),str(image)],check=True)
print(('Native' if a.native else 'JIT')+(' capacity and network readiness passed' if a.capacity_only else ' local IPC passed' if a.local else ' select: API, files/pipes/PTYs, signals/deadlines, keyboard, idle CPU and TCP/UDP/listener readiness passed'))
