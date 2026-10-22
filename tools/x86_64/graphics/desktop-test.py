#!/usr/bin/env python3
"""Boot only a disposable desktop fixture; exercise real RXE1 apps via UART/QMP."""
import argparse
import json
import os
from pathlib import Path
import select
import shlex
import shutil
import socket
import subprocess
import time

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fixture',type=Path,required=True)
    p.add_argument('--accel',choices=('tcg','kvm'),default='tcg')
    a=p.parse_args();w=a.fixture.resolve()
    if json.loads((w/'candidate.json').read_text())['platform']!='fixture':
        raise RuntimeError('only the credential-free fixture profile may be booted here')
    qpath=w/'qmp.sock';qpath.unlink(missing_ok=True)
    shutil.copyfile(w/'edk2-i386-vars.fd',w/'vars.fd')
    cmd=['qemu-system-x86_64','-machine','q35','-accel',a.accel,'-cpu','host' if a.accel=='kvm' else 'qemu64',
         '-smp','1','-m','1536M','-no-reboot','-nic','none','-monitor','none','-display','none','-S',
         '-drive','if=pflash,format=raw,readonly=on,file='+str(w/'edk2-x86_64-code.fd'),
         '-drive','if=pflash,format=raw,file='+str(w/'vars.fd'),
         '-device','qemu-xhci','-drive','if=none,id=bootusb,format=raw,snapshot=on,file='+str(w/'usb-boot.img'),
         '-device','usb-storage,drive=bootusb,removable=on,bootindex=1',
         '-drive','if=none,id=rootdisk,format=raw,file='+str(w/'root.img'),
         '-device','virtio-blk-pci,drive=rootdisk,disable-legacy=on',
         '-device','isa-debug-exit,iobase=0xf4,iosize=0x04','-serial','stdio',
         '-qmp','unix:'+str(qpath)+',server=on,wait=off']
    started=time.monotonic();text=bytearray();pending=bytearray();qs=socket.socket(socket.AF_UNIX)
    qs.settimeout(20)
    log=(w/'qemu.log').open('wb');serial=(w/'serial.log').open('wb')
    proc=subprocess.Popen(cmd,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,bufsize=0)
    def pump(wait=.02):
        if select.select([proc.stdout],[],[],wait)[0]:
            chunk=os.read(proc.stdout.fileno(),65536)
            if chunk:text.extend(chunk);serial.write(chunk);serial.flush()
        return text.decode(errors='replace')
    def settle(seconds):
        end=time.monotonic()+seconds
        while time.monotonic()<end:pump()
    def wait_for(token,after=0,timeout=150):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            output=pump()
            for fail in ('BOOT_FAIL','[KERNEL PANIC]','EXCEPTION vector=','LVGL: graphics operation failed','GX_CREATE:'):
                if fail in output:raise RuntimeError(output[-2000:])
            if token.encode() in text[after:]:return output
            if proc.poll() is not None:raise RuntimeError('QEMU exited: '+output[-2000:])
        raise RuntimeError('timeout '+repr(token)+': '+pump(0)[-2000:])
    def packet():
        while b'\n' not in pending:
            data=qs.recv(65536)
            if not data:raise RuntimeError('QMP EOF')
            pending.extend(data)
        line,_,rest=pending.partition(b'\n');pending[:]=rest;return json.loads(line)
    def qmp(name,args=None):
        data={'execute':name}
        if args is not None:data['arguments']=args
        qs.sendall(json.dumps(data).encode()+b'\n')
        while True:
            value=packet()
            if 'error' in value:raise RuntimeError(str(value['error']))
            if 'return' in value:return value['return']
    def uart(line):proc.stdin.write((line+'\n').encode());proc.stdin.flush()
    def published(title):
        start=len(text);uart('desktop-probe --wait '+shlex.quote(title))
        wait_for('GX_PUBLISHED '+title,start);settle(.5)
    def visible_panel():
        header,dimensions,maximum,pixels=(w/'desktop.ppm').read_bytes().split(b'\n',3)
        if header!=b'P6' or maximum!=b'255':raise RuntimeError('unexpected QEMU screenshot format')
        width,height=map(int,dimensions.split());background=pixels[:3]
        def changed(x,y,w,h):
            return sum(pixels[(row*width+col)*3:(row*width+col)*3+3]!=background
                       for row in range(y,y+h) for col in range(x,x+w))
        if changed(12,8,94,30)<500 or changed(width//2-100,height-60,200,40)<500:
            raise RuntimeError('FuseDE panel was mapped but its brand/dock pixels were not rendered')
    def keys(*codes):
        events=[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':code}}}
                for code,down in [(x,True) for x in codes]+[(x,False) for x in reversed(codes)]]
        qmp('input-send-event',{'events':events});settle(.18)
    try:
        while not qpath.exists():
            if proc.poll() is not None or time.monotonic()>started+10:raise RuntimeError('QEMU startup failed')
            time.sleep(.02)
        qs.connect(str(qpath));assert 'QMP' in packet();qmp('qmp_capabilities');qmp('cont')
        wait_for('fuse-x# ')
        uart('fusedeshell --trace &');wait_for('FUSEDE_READY');published('FuseDE')
        qmp('screendump',{'filename':str(w/'desktop.ppm')})
        visible_panel()
        start=len(text);uart('desktop-probe --idle');wait_for('GX_IDLE_OK',start)
        uart('lvcalc --trace &');wait_for('CALC_READY');published('Calculator')
        keys('2');keys('shift','equal');keys('3');keys('ret');wait_for('CALC_RESULT 5')
        uart('lvedit --trace /root/hello.txt &');wait_for('EDIT_READY');published('Text Editor')
        qmp('screendump',{'filename':str(w/'editor.ppm')})
        keys('ctrl','q');settle(1)
        start=len(text);keys('esc');keys('7');keys('shift','equal');keys('8');keys('ret');wait_for('CALC_RESULT 15',start)
        uart('lvdata --trace --plot /root/measurements.csv &');wait_for('DATA_READY');published('Data Viewer')
        qmp('screendump',{'filename':str(w/'data.ppm')})
        keys('alt','f4');settle(1)
        uart('gterm /bin/sh -c "echo NATIVE_GRAPHICAL_TERMINAL; sleep 8" &');published('Terminal')
        qmp('screendump',{'filename':str(w/'terminal.ppm')});settle(9)
        uart('shutdown');output=wait_for('RIPLIX_X86_64_BOOT_OK')
        code=proc.wait(timeout=15)
        if code!=33 or 'CONTEXT_PAGES_OK' not in output:raise RuntimeError('shutdown/page accounting failed')
        report={'status':'passed','accelerator':a.accel,'seconds':round(time.monotonic()-started,3),
                'candidate':json.loads((w/'candidate.json').read_text())['efi_sha256'],
                'checks':['FuseDE native alpha panel and catalog','stable idle registry revision','calculator physical keyboard arithmetic',
                          'editor file load and cooperative close','focus returns to calculator',
                          'data viewer and plot','graphical terminal','shutdown and context-page accounting'],
                'physical_acceptance':False}
        (w/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
    except Exception as error:
        (w/'failure.json').write_text(json.dumps({'error':str(error),'seconds':time.monotonic()-started},indent=2)+'\n');raise
    finally:
        if proc.poll() is None:proc.kill();proc.wait()
        serial.close();log.close();qs.close()

if __name__=='__main__':main()
