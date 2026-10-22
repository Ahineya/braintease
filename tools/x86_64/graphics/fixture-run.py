#!/usr/bin/env python3
"""Run only the disposable GUI fixture in a single-core KVM VM."""
import hashlib,json,os,select,shutil,socket,subprocess,time
from pathlib import Path
w=Path(__file__).resolve().parent
qmp_path=w/'qmp.sock';qmp_path.unlink(missing_ok=True)
vars_path=w/'vars.fd';shutil.copyfile(w/'edk2-i386-vars.fd',vars_path)
command=['qemu-system-x86_64','-machine','q35','-accel','kvm','-cpu','host','-smp','1','-m','1024M','-no-reboot','-nic','none','-monitor','none','-display','none','-S',
 '-drive','if=pflash,format=raw,readonly=on,file='+str(w/'edk2-x86_64-code.fd'),'-drive','if=pflash,format=raw,file='+str(vars_path),
 '-device','qemu-xhci','-drive','if=none,id=bootusb,format=raw,snapshot=on,file='+str(w/'usb-boot.img'),'-device','usb-storage,drive=bootusb,removable=on,bootindex=1',
 '-drive','if=none,id=rootdisk,format=raw,file='+str(w/'root.img'),'-device','virtio-blk-pci,drive=rootdisk,disable-legacy=on',
 '-device','isa-debug-exit,iobase=0xf4,iosize=0x04','-serial','stdio','-qmp','unix:'+str(qmp_path)+',server=on,wait=off']
log=(w/'qemu.log').open('wb');serial=(w/'serial.log').open('wb')
proc=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,bufsize=0)
started=time.monotonic();deadline=started+180;text=bytearray();qs=socket.socket(socket.AF_UNIX);pending=bytearray()
def pump(wait=.01):
    if select.select([proc.stdout],[],[],wait)[0]:
        chunk=os.read(proc.stdout.fileno(),65536)
        if chunk:text.extend(chunk);serial.write(chunk);serial.flush()
    return text.decode(errors='replace')
def wait_for(token,timeout=90):
    end=min(deadline,time.monotonic()+timeout)
    while time.monotonic()<end:
        output=pump(.02)
        if 'BOOT_FAIL' in output or '[KERNEL PANIC]' in output or 'EXCEPTION vector=' in output or 'GX_CREATE:' in output or 'LVGL: graphics operation failed' in output:raise RuntimeError('guest failed: '+output[-1400:])
        if token in output:return output
        if proc.poll() is not None:raise RuntimeError('QEMU exited: '+output[-1400:])
    raise RuntimeError('timeout waiting '+repr(token)+'; '+pump(0)[-1400:])
def packet():
    while b'\n' not in pending:
        if not select.select([qs],[],[],max(.1,deadline-time.monotonic()))[0]:raise RuntimeError('QMP timeout')
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
def uart(line):
    proc.stdin.write((line+'\n').encode());proc.stdin.flush()
def key(code,shift=False):
    events=[]
    for k,d in ([('shift',True)] if shift else [])+[(code,True),(code,False)]+([('shift',False)] if shift else []):
        events.append({'type':'key','data':{'down':d,'key':{'type':'qcode','data':k}}})
    qmp('input-send-event',{'events':events})
    end=time.monotonic()+.12
    while time.monotonic()<end:pump(.01)
try:
    while not qmp_path.exists():
        if proc.poll() is not None or time.monotonic()>started+10:raise RuntimeError('QEMU did not start')
        time.sleep(.01)
    qs.connect(str(qmp_path));assert 'QMP' in packet();qmp('qmp_capabilities');qmp('cont')
    wait_for('fuse-x# ')
    uart('lvcalc --trace &');wait_for('CALC_READY')
    # Allow the complete first surface to publish before keyboard input.
    end=time.monotonic()+1
    while time.monotonic()<end:pump(.01)
    key('2');key('equal',True);key('3');key('ret')
    wait_for('CALC_RESULT 5')
    uart('screenshot /tmp/graphics.ppm');wait_for('Screenshot saved: /tmp/graphics.ppm')
    qmp('screendump',{'filename':str(w/'qmp.ppm')})
    # Escape clears this calculator; it is not the application's quit shortcut.
    key('esc')
    # Standard native window shortcut supplies GX_CLOSE; LVGL returns normally.
    qmp('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':code}}} for code,down in [('alt',True),('f4',True),('f4',False),('alt',False)]]})
    end=time.monotonic()+.5
    while time.monotonic()<end:pump(.01)
    uart('echo GRAPHICS_CLOSED');wait_for('GRAPHICS_CLOSED\r\n')
    qmp('screendump',{'filename':str(w/'console.ppm')})
    uart('shutdown');output=wait_for('RIPLIX_X86_64_BOOT_OK')
    code=proc.wait(timeout=10)
    if code!=33:raise RuntimeError('unexpected QEMU exit '+str(code))
    if 'CONTEXT_PAGES_OK' not in output:raise RuntimeError('page accounting acceptance missing')
    report={'status':'passed','accelerator':'kvm','cpus':1,'memory_mib':1024,'seconds':round(time.monotonic()-started,3),
            'checks':['virtio persistent disposable root','native GX calculator start','QMP physical keyboard 2+3=5','native screenshot while graphical app running','Escape clear key','Alt+F4 normal GUI exit and console restoration','filesystem sync and context physical-page guard'],
            'efi_fixture':json.loads((w/'fixture.json').read_text()) if (w/'fixture.json').exists() else {},'qemu_exit':code,
            'limitation':'row-wise screenshot parity is checked after extracting the disposable guest file; screenshots do not promise atomic moving content'}
    (w/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report),flush=True)
except Exception as error:
    (w/'failure.json').write_text(json.dumps({'error':str(error),'seconds':time.monotonic()-started},indent=2)+'\n');raise
finally:
    if proc.poll() is None:proc.kill();proc.wait()
    serial.close();log.close();qs.close()
