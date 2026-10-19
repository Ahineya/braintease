#!/usr/bin/env python3
"""Real standalone Rift/PTY checks with isolated HOME and disposable host files."""
import argparse
import fcntl
import os
from pathlib import Path
import pty
import select
import signal
import shutil
import statistics
import struct
import subprocess
import tempfile
import termios
import time
from test_rift_unicode import UnicodeScreen

ROOT=Path(__file__).resolve().parents[2]


class Editor:
    def __init__(self,executable,home,files=(),rows=30,columns=80):
        self.master,self.slave=pty.openpty()
        self.original=termios.tcgetattr(self.slave)
        self.screen=UnicodeScreen();self.screen.resize(rows,columns)
        fcntl.ioctl(self.slave,termios.TIOCSWINSZ,struct.pack('HHHH',rows,columns,0,0))
        self.log=bytearray()
        env=dict(os.environ,HOME=str(home),TERM='xterm-256color',RCC_NATIVE_AUDIO='off',RCC_NATIVE_NET='off')
        env.pop('RCC_NATIVE_TIMEOUT',None)
        self.process=subprocess.Popen([str(executable),*map(str,files)],stdin=self.slave,stdout=self.slave,stderr=self.slave,
                                      cwd=home,env=env,start_new_session=True)
        os.set_blocking(self.master,False)
    def pump(self,seconds=.02):
        if select.select([self.master],[],[],seconds)[0]:
            try:data=os.read(self.master,65536)
            except OSError:data=b''
            self.log.extend(data);self.screen.feed(data)
    def wait(self,predicate,label,seconds=30):
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            self.pump()
            if predicate():return
            if self.process.poll() is not None:break
        raise AssertionError(label+'\n'+self.screen.text()+'\n'+self.log[-1500:].decode(errors='replace'))
    def visible(self,text,seconds=30):self.wait(lambda:text in self.screen.text(),text,seconds)
    def send(self,data):
        at=0
        until=time.monotonic()+30
        while at<len(data):
            assert time.monotonic()<until,'PTY input timed out'
            try:at+=os.write(self.master,data[at:at+1024])
            except BlockingIOError:pass
            self.pump(.001)
    def colon(self,text):
        self.send(b':');self.visible(': command');self.send(text.encode()+b'\r')
    def resize(self,rows,columns):
        self.screen.resize(rows,columns)
        fcntl.ioctl(self.master,termios.TIOCSWINSZ,struct.pack('HHHH',rows,columns,0,0))
        os.kill(self.process.pid,signal.SIGWINCH)
    def finish(self):
        self.wait(lambda:self.process.poll() is not None,'editor exit',15)
        while select.select([self.master],[],[],.05)[0]:
            try:data=os.read(self.master,65536)
            except OSError:break
            if not data:break
            self.log.extend(data)
        after=termios.tcgetattr(self.slave)
        # Darwin may set the driver's pending-input retype bit when returning
        # to canonical mode. It is not an application-selected terminal mode.
        after[3]&=~getattr(termios,'PENDIN',0)
        self.original[3]&=~getattr(termios,'PENDIN',0)
        assert after==self.original,('Terminal was not restored',self.original,after)
        assert b'\x1b[?1049l' in self.log and b'\x1b[?2004l' in self.log,'alternate screen/paste mode not restored'
    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            until=time.monotonic()+10
            while self.process.poll() is None and time.monotonic()<until:self.pump(.05)
            if self.process.poll() is None:self.process.kill()
            until=time.monotonic()+5
            while self.process.poll() is None and time.monotonic()<until:self.pump(.05)
        os.close(self.master);os.close(self.slave)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable',type=Path,default=ROOT/'build/rift-macos/bin/rift')
    args=parser.parse_args();exe=args.executable.resolve()
    out=ROOT/'build/rift-macos/tests';out.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='rift-macos-') as temporary:
        home=Path(temporary)
        path=home/('long-host-filename-'+'x'*65+'.txt')
        original='Привіт e\u0301 中 👩\u200d💻\nsecond line\n'
        path.write_text(original)
        editor=Editor(exe,home,[path]);started=time.monotonic()
        try:
            editor.visible('NORMAL');startup=time.monotonic()-started
            editor.visible('Привіт')
            editor.send(b'i');editor.visible('INSERT')
            payload='Native paste: '+('Український текст e\u0301 中 👩\u200d💻. '*100)+'\nLiteral controls: \x1b:q! \x13\nPASTE_DONE_END\n'
            before=time.monotonic()
            editor.send(b'\x1b[200~'+payload.encode()+b'\x1b[201~')
            editor.visible('PASTE_DONE_END');paste=time.monotonic()-before
            editor.send(b'\x1b');editor.visible('NORMAL')
            editor.colon('w');editor.visible('Saved ')
            editor.wait(lambda:path.read_text()==payload+original,'saved UTF-8 paste')
            # Undo/redo use their real vi keys rather than command aliases.
            editor.send(b'u');editor.colon('w');editor.visible('Saved ')
            editor.wait(lambda:path.read_text()==original,'one undo removes bulk paste')
            editor.send(b'\x12');editor.colon('w');editor.visible('Saved ')
            editor.wait(lambda:path.read_text()==payload+original,'redo restores bulk paste')
            editor.resize(40,120)
            editor.wait(lambda:'F1 Help' in ''.join(editor.screen.lines[39]),'120x40 resize')
            editor.colon('set nowrap');editor.visible('Settings saved')
            assert 'wrap=false' in (home/'.riftrc').read_text()
            # The native directory reader must retain filenames longer than 30 bytes.
            editor.send(b'\x0f');editor.visible('Open')
            editor.visible(path.name[:55])
            editor.send(b'\x1b');editor.visible('NORMAL')
            editor.colon('theme');editor.visible('Theme (session)')
            lines=[''.join(row) for row in editor.screen.lines]
            first=next(i for i,line in enumerate(lines) if 'configured' in line)
            graphite=next(i for i,line in enumerate(lines) if 'graphite' in line)
            editor.send(b'\x1b[B'*(graphite-first)+b'\r')
            editor.visible('NORMAL')
            editor.wait(lambda:editor.screen.styles[2][7][1]==0,'graphite theme colors')
            editor.colon('qa!');editor.finish()
            assert editor.process.returncode==0
            (out/'editor.ansi').write_bytes(editor.log)
            print(f'Native Rift: startup {startup:.3f}s; {len(payload.encode())}-byte Unicode paste {paste:.3f}s; save/undo/redo/resize/themes/long names passed',flush=True)
        finally:
            editor.close()
            (out/'last-editor.ansi').write_bytes(editor.log)
        # Settings survive a fresh host process. No profile file outside HOME is touched.
        relocated=home/'relocated'
        (relocated/'bin').mkdir(parents=True)
        shutil.copy2(exe,relocated/'bin/rift')
        shutil.copytree(exe.parent.parent/'share',relocated/'share')
        editor=Editor(relocated/'bin/rift',home,[path])
        try:
            editor.visible('NORMAL')
            editor.send(b'$')
            editor.wait(lambda:editor.screen.y==2 and editor.screen.x>7,'restored nowrap setting')
            editor.colon('theme');editor.visible('Theme (session)')
            lines=[''.join(row) for row in editor.screen.lines]
            first=next(i for i,line in enumerate(lines) if 'configured' in line)
            graphite=next(i for i,line in enumerate(lines) if 'graphite' in line)
            editor.send(b'\x1b[B'*(graphite-first)+b'\r');editor.visible('NORMAL')
            editor.wait(lambda:editor.screen.styles[2][7][1]==0,'relocated theme resources')
            editor.colon('set wrap');editor.visible('Settings saved')
            assert 'wrap=true' in (home/'.riftrc').read_text()
            editor.colon('qa!');editor.finish()
        finally:editor.close()
        script=home/'run-me.sh';script.write_text('echo NATIVE_HOST_CHILD_OK\n')
        editor=Editor(exe,home,[script])
        try:
            editor.visible('NORMAL')
            editor.send(b'\x1b[15~') # F5: save/run using the host shell
            editor.wait(lambda:b'NATIVE_HOST_CHILD_OK' in editor.log and b'Press Enter to return' in editor.log,'host command execution')
            editor.send(b'\r');editor.visible('NORMAL')
            editor.colon('qa!');editor.finish()
            assert editor.process.returncode==0
            (out/'run-command.ansi').write_bytes(editor.log)
        finally:editor.close()
        editor=Editor(exe,home)
        try:
            editor.visible('NORMAL');editor.process.terminate();editor.finish()
            assert editor.process.returncode==128+signal.SIGTERM
        finally:editor.close()
    print('Native Rift: host commands, settings reload, relocatable resources, normal/signal terminal restoration passed',flush=True)


if __name__=='__main__':main()
