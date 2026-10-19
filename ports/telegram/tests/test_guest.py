#!/usr/bin/env python3
"""Run the real Telegram C engine over guest Ethernet against a local oracle."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
from support import ROOT, PORT, BUILD, build
from test_login import Peer

sys.path.insert(0, str(ROOT / 'fuse-x'))
from test_network import Services
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest


class WireOracle(Services):
    def __init__(self, path, server):
        super().__init__(path)
        self.server = server
        self.tcp_services[7100] = self.transfer

    def transfer(self, stream):
        if not hasattr(stream, 'upstream'):
            stream.upstream = socket.create_connection(('127.0.0.1', self.server.port), timeout=3)
            stream.upstream.setblocking(False)
            stream.forwarded, stream.upout, stream.up_eof, stream.up_shutdown = 0, bytearray(), False, False
        fresh = stream.received[stream.forwarded:]
        stream.forwarded = len(stream.received)
        stream.upout.extend(fresh)

    def pump(self):
        super().pump()
        for stream in self.streams.values():
            if not hasattr(stream, 'upstream'):
                continue
            if stream.upout:
                try:
                    done = stream.upstream.send(stream.upout)
                    del stream.upout[:done]
                except BlockingIOError:
                    pass
            if stream.fin and not stream.upout and not stream.up_shutdown:
                stream.upstream.shutdown(socket.SHUT_WR)
                stream.up_shutdown = True
            if not stream.up_eof:
                for _ in range(4):
                    try:
                        data = stream.upstream.recv(4096)
                        if not data:
                            stream.up_eof = True
                            break
                        stream.queue(data)
                    except BlockingIOError:
                        break
            if stream.up_eof and not stream.pending and not stream.flight:
                stream.closing = True
                stream.progress()

    def close(self):
        for stream in self.streams.values():
            if hasattr(stream, 'upstream'):
                stream.upstream.close()
        super().close()


class TelegramGuest(Guest):
    def pump(self):
        if b'TG_CHAT_BOB_READY' in self.log and not getattr(self,'chat_pushed',False):
            self.chat_pushed=True;self.peer.server.push()
        if b'TG_QR_READY' in self.log:
            self.peer.server.approve.set()
        if b'TG_SENT' in self.log:
            self.peer.server.sent.set()
        if b'TG_RESTORED' in self.log:
            self.peer.server.restored.set()
        if not self.peer.server.errors.empty():
            raise self.peer.server.errors.get()
        super().pump()


def command(args, **kwargs):
    result = subprocess.run(list(map(str, args)), capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(result.stdout.decode(errors='replace')[-3000:] + result.stderr.decode(errors='replace')[-5000:])
    return result.stdout


def guest_build(target, ui=False, chats=False):
    artifact = BUILD / (('ui' if ui else 'chats' if chats else 'probe') + ('.fxe' if target == 'native' else '.rxe'))
    units = [PORT / (name+'.c') for name in ('wire','hash','inflate','tg_crypto','packet','keys','storage','client','handshake','account','receive','tl','tl_schema','model','dialogs','updates')]
    units += [BUILD/'fixture_keys.c']
    if ui:
        ir = BUILD/'telegram-ui.ir'
        ir.write_bytes(command([ROOT/'target/release/rfl','compile',ROOT/'rfl/apps/telegram/main.rfl',
                               '--emit-ir','--target',target]))
        units += [ir, ROOT/'rfl/os/posix.c',ROOT/'rfl/os/curses.c',ROOT/'libraries/tui-theme/tui_theme.c']
    else:
        units += [PORT/('tests/chat_probe.c' if chats else 'tests/guest_probe.c')]
    units += [ROOT/'rfl/os'/name for name in ('socket.c','poll.c','dns.c','crypto.c')]
    if target == 'native':
        command([sys.executable, ROOT/'fuse-x/cc.py', *units, '-DTG_TESTING', '-DTG_TEST_KEYS', '-I', PORT, '-o', artifact,*(['--curses'] if ui else [])])
    else:
        sdk_build = ROOT/'build/fuzix'
        command(['make','--no-print-directory','-C',ROOT/'ports/fuzix',f'BUILD={sdk_build}',f'RFL_CURSES={int(ui)}','rfl-sdk'])
        sdk = json.loads(command(['make','-s','--no-print-directory','-C',ROOT/'ports/fuzix',f'BUILD={sdk_build}',f'RFL_CURSES={int(ui)}','rfl-sdk-info']))
        flags = ['--bank-size','64000','-DRIPPLE_MMU=1','-DNO_64BIT','-DTG_TESTING','-DTG_TEST_KEYS']
        for path in [PORT,ROOT/'ports/fuzix/ripple/include',ROOT/'ports/fuzix/upstream/Library/include',ROOT/'runtime/include',ROOT/'ports/fuzix/ripple']:
            flags += ['-I',str(path)]
        def unit(item):
            i, source = item
            asm, obj = BUILD/f'unit-{i}.asm', BUILD/f'unit-{i}.pobj'
            command([ROOT/'target/release/rcc','compile',source,*flags,'-o',asm])
            command([ROOT/'src/ripple-asm/target/release/rasm','assemble',asm,'-o',obj,'--bank-size','64000','--max-immediate','65535'])
            return obj
        with ThreadPoolExecutor(max_workers=4) as pool:
            objects = list(pool.map(unit, enumerate(units)))
        command([ROOT/'src/ripple-asm/target/release/rlink',sdk['entry'],*objects,*sdk['libraries'],'-f','rxe','--bank-size','64000','-o',artifact])
    print(f'Built {target} Telegram fixture', flush=True)
    return artifact


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',choices=('native','ripple','both'),default='native')
    parser.add_argument('--chats',action='store_true')
    args = parser.parse_args()
    lib, rsa, prime = build()
    for target in (('native','ripple') if args.target=='both' else (args.target,)):
        artifact = guest_build(target,chats=args.chats)
        if args.chats:
            from chat_peer import ChatPeer
            server=ChatPeer(lib,rsa,prime)
        else:server = Peer(lib,rsa,prime)
        server.start()
        with tempfile.TemporaryDirectory(prefix='tg-guest-',dir='/tmp') as folder:
            peer = WireOracle(Path(folder)/'nic',server)
            guest = None
            try:
                guest = TelegramGuest(folder,target,{'tg-probe':artifact},files=[dict(name='/etc/telegram.conf',mode=0o100600,
                    text='api_id=12345\napi_hash='+'0'*32+'\n')],peer=peer)
                output = guest.shell('tg-probe; echo TG_STATUS:$?',180)
                print(output,flush=True)
                assert 'TG_GUEST_OK' in output and 'TG_STATUS:0' in output,output
                assert server.received == [b'Hello from the isolated C16 Telegram fixture']
                if args.chats:assert server.recipients==[('User',8)]
                (BUILD/f'guest-{target}.log').write_bytes(guest.log)
            finally:
                if guest:
                    guest.close()
                peer.close()
        server.join(5)
        assert not server.is_alive()
        if not server.errors.empty():
            raise server.errors.get()


if __name__=='__main__':
    main()
