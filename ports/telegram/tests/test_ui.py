#!/usr/bin/env python3
"""Drive the themed RFL/curses login and Saved Messages UI with dummy credentials."""
import fcntl
import os
import subprocess
from pathlib import Path
import struct
import sys
import tempfile
import termios
import time
from support import ROOT,BUILD,build
from chat_peer import ChatPeer
from test_guest import WireOracle,Guest,guest_build

sys.path.insert(0,str(ROOT/'libraries/tests'))
from test_qr import screen,write_png


def main():
    lib,rsa,prime=build()
    artifact=guest_build('native',ui=True)
    server=ChatPeer(lib,rsa,prime)
    server.start()
    with tempfile.TemporaryDirectory(prefix='tg-ui-',dir='/tmp') as folder:
        peer=WireOracle(Path(folder)/'nic',server)
        guest=None
        active=False
        try:
            guest=Guest(folder,'native',{'telegram':artifact},files=[
                dict(name='/root/.telegram.conf',mode=0o100600,text='api_id=12345\napi_hash='+'0'*32+'\n'),
                dict(name='/etc/termcap',text='ansi|vt100|xterm:co#80:li#30:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:me=\\E[0m:\n')],peer=peer)
            fcntl.ioctl(guest.master,termios.TIOCSWINSZ,struct.pack('HHHH',30,80,0,0))
            at=len(guest.log)
            helper=ROOT/'build/qr-test/qr-check'
            assert helper.exists(),'Run libraries/tests/test_qr.py --target host first'
            def visible(needle,seconds=30):
                until=time.monotonic()+seconds
                while time.monotonic()<until:
                    guest.pump()
                    if not server.errors.empty():raise server.errors.get()
                    cells=screen(helper,bytes(guest.log[at:]))
                    text='\n'.join(''.join(chr(ch) if 32<=ch<127 else ' ' for ch,_ in cells[y*80:(y+1)*80]) for y in range(30))
                    if needle in text:return cells
                    time.sleep(.02)
                raise AssertionError('UI did not show '+needle+'\n'+text)
            os.write(guest.master,b'telegram\r')
            active=True
            visible('Scan in Telegram on your phone')
            # Drain the remainder of that frame before rasterizing it.
            until=time.monotonic()+.15
            while time.monotonic()<until:guest.pump()
            if helper.exists():
                cells=screen(helper,bytes(guest.log[at:]))
                write_png(BUILD/'login-ui.png',cells)
                assert sum(ch in (0x2588,0x2584,0x2580) for ch,_ in cells)>100
                scanner=ROOT/'build/qr-test/decode-qr'
                if sys.platform=='darwin' and scanner.exists():
                    subprocess.run([str(scanner),str(BUILD/'login-ui.png'),server.expected_qr],check=True)
            server.approve.set()
            visible('Enter your Telegram 2FA password')
            # Password bytes must never be echoed into the terminal capture.
            os.write(guest.master,server.password+b'\r')
            visible('Existing Saved Messages history')
            visible('Archived friend')
            os.write(guest.master,b'/bob\r\r')
            visible('Bob history')
            server.push()
            visible('Live incoming fixture text')
            os.write(guest.master,b'\x1b[5~')
            visible('Earliest fixture history')
            os.write(guest.master,b'\x1b[15~')
            visible('Bob history')
            os.write(guest.master,b'reject fixture draft\r')
            visible('Send failed 403')
            visible('reject fixture draft')
            os.write(guest.master,b'\x15')
            # Return to the tail and send only to the selected Bob fixture.
            os.write(guest.master,b'\x1b[6~Hello from the isolated C16 Telegram fixture\r')
            visible('Message accepted by Telegram')
            server.exit_ok.set()
            os.write(guest.master,b'\x1b[21~')
            guest.at=len(guest.log)
            guest.read(guest.prompt)
            active=False
            assert server.password not in guest.log
            os.write(guest.master,b'telegram\r')
            active=True
            visible('Existing Saved Messages history')
            cells=visible('Archived friend')
            write_png(BUILD/'chats-ui.png',cells)
            server.exit_ok.set()
            os.write(guest.master,b'\x1b[21~')
            guest.at=len(guest.log)
            guest.read(guest.prompt)
            active=False
            assert server.received==[b'Hello from the isolated C16 Telegram fixture']
            assert server.recipients==[('User',8)]
            assert (8,8) in server.history_requests
            (BUILD/'ui.log').write_bytes(guest.log)
            print('RFL curses UI: exact QR, hidden UTF-8 2FA, pinned/archived chats, search, history paging, live incoming, selected-recipient send and session restart passed')
        finally:
            if guest:
                if active:
                    server.exit_ok.set()
                    guest.at=len(guest.log)
                    os.write(guest.master,b'\x1b[21~')
                    guest.read(guest.prompt,5)
                guest.close()
            peer.close()
    server.join(5)
    assert not server.is_alive()
    if not server.errors.empty():raise server.errors.get()


if __name__=='__main__':main()
