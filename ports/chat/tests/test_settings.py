#!/usr/bin/env python3
"""Custom instruction replay and compact UI on real, disposable guest systems."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import signal
import struct
import sys
import tempfile
import termios
import time
from test_guest import ROOT, HOST, MESSAGE, Service, Guest, make

PROFILE = 'Call me Pavlo. Keep "quotes", \\slashes and Ж🙂.\n'
REPLACEMENT = 'Updated preferences, without restarting.\n'


class Settings(Service):
    def completion(self, request):
        prompt = request['input'][-1]['content']
        instructions = request['instructions']
        expected = PROFILE if prompt in ('plain-custom', 'workspace-custom', 'workspace-kept') else \
            REPLACEMENT if prompt == 'workspace-reloaded' else None
        if expected:
            assert instructions.endswith(expected), instructions
            assert 'User-provided custom instructions:' in instructions
        else:
            assert 'User-provided custom instructions:' not in instructions, instructions
        assert ('tools' in request) == prompt.startswith('workspace-'), request
        return super().completion(request)


def run(target, ui_only=False):
    build = ROOT / ('fuse-x/build' if target == 'native' else 'build/fuzix')
    ext = 'fxe' if target == 'native' else 'rxe'
    apps = {name: build / (f'apps/{name}.{ext}' if target == 'native' else f'{name}.{ext}')
            for name in ('chat', 'chmod')}
    with tempfile.TemporaryDirectory(prefix='chat-settings-') as temporary:
        work = Path(temporary)
        certs = make(work / 'certs')
        peer = Settings(work / 'nic', certs)
        invalid = work / 'invalid'
        invalid.write_bytes(b'\xff')
        account = dict(issuer='https://auth.openai.com', subject='fixture-user', client_id='oaiapp_fixture',
                       ext_agent_host_id=HOST, token_type='Bearer', access_token='fixture-expired',
                       refresh_token='fixture-refresh', expires_at=1,
                       scopes=['resource.invoke', 'chatgpt.tokens.use.direct'])
        files = [dict(name='/etc/ssl/cert.pem', path=str(certs / 'ca.pem')),
                 dict(name='/chat/host-id', text=HOST, mode=0o100600),
                 dict(name='/credentials.json', text=json.dumps(account), mode=0o100600),
                 dict(name='/profile', text=PROFILE), dict(name='/replacement', text=REPLACEMENT),
                 dict(name='/bad', path=str(invalid)), dict(name='/empty', text=''),
                 dict(name='/work/README', text='Workspace\n'),
                 dict(name='/etc/termcap', text='ansi|vt100|xterm:co#80:li#24:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:ce=\\E[K:up=\\E[A:bc=^H:\n')]
        if ui_only:
            history = [dict(role='user', content='compact-layout'), MESSAGE]
            files += [dict(name='/chat/c000001.jsonl', mode=0o100600,
                           text=''.join(json.dumps(item) + '\n' for item in history)),
                      dict(name='/chat/c000001.workspace', text='/work', mode=0o100600)]
        guest = Guest(work, target, apps, files, peer, blocks=24576, disk_sectors=131072)
        active = False
        try:
            guest.shell('chmod 700 /chat')
            env = 'CHAT_HOME=/chat CHAT_API_ORIGIN=https://tls.test CHAT_AUTH_ORIGIN=https://tls.test '
            if not ui_only:
                assert 'IMPORT:0' in guest.shell(env + 'chat --import /credentials.json; echo IMPORT:$?')

                def ask(prompt, workspace=False):
                    command = 'chat --workspace /work' if workspace else 'chat'
                    result = guest.shell(env + command + f' --ask {prompt}; echo ASK:$?', 300)
                    assert 'ASK:0' in result and 'Reply saved' in result, result

                ask('plain-none')
                result = guest.shell(env + 'chat --instructions /profile; echo PROFILE:$?')
                assert 'PROFILE:0' in result, result
                assert PROFILE.rstrip() in guest.shell('cat /chat/.instructions')
                ask('plain-custom')
                ask('workspace-custom', workspace=True)
                result = guest.shell(env + 'chat --instructions /bad; echo INVALID:$?')
                assert 'INVALID:1' in result and 'UTF-8' in result, result
                ask('workspace-kept')
                guest.shell('cat /replacement > /chat/.instructions')
                ask('workspace-reloaded')
                guest.shell('cat /bad > /chat/.instructions')
                calls = len(peer.calls)
                result = guest.shell(env + 'chat --ask invalid-local; echo INVALID:$?', 60)
                assert 'INVALID:1' in result and 'Cannot read .instructions' in result, result
                assert len(peer.calls) == calls
                guest.shell('cat /empty > /chat/.instructions')
                ask('workspace-cleared')
                print(target + ': custom instructions, Unicode, plain/workspace replay, reload and invalid-file handling passed', flush=True)

            sys.path.insert(0, str(ROOT / 'rfl/tools'))
            from test_os import Screen
            screen, painted = Screen(), len(guest.log)

            def shown(marker, absent=None, timeout=30):
                nonlocal painted
                end = time.monotonic() + timeout
                while time.monotonic() < end:
                    guest.pump()
                    screen.feed(bytes(guest.log[painted:])); painted = len(guest.log)
                    if marker in screen.text() and (absent is None or absent not in screen.text()):
                        return
                raise AssertionError(f'UI missing {marker!r}, excluding {absent!r}\n' + screen.text())

            os.write(guest.master, (env + 'chat\r').encode()); active = True
            shown('Conversations')
            shown('Second line.')
            assert any(line.find('Second line.') == 22 for line in screen.text().splitlines()), screen.text()
            os.write(guest.master, b'\x1bOR')  # F3 passes through mux without its Ctrl-B prefix.
            shown('Second line.', absent='Conversations')
            assert any(line.startswith('  Second line.') for line in screen.text().splitlines()), screen.text()
            os.write(guest.master, b'\t')
            shown('Conversations')
            os.write(guest.master, b'\r')
            shown('Second line.', absent='Conversations')
            os.write(guest.master, b'\x04'); guest.read(guest.prompt, 30); active = False
            assert 'hidden' in guest.shell('cat /chat/.sidebar')
            os.write(guest.master, (env + 'chat\r').encode()); active = True
            shown('Second line.', absent='Conversations')

            # Use a 40-column pane, as in an 80-column terminal split in half.
            screen.resize(24, 40)
            fcntl.ioctl(guest.master, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 40, 0, 0))
            os.kill(guest.process.pid, signal.SIGWINCH)
            os.write(guest.master, b'NARROW_COMPOSER')
            shown('NARROW_COMPOSER', absent='Please enlarge')
            shown('F3/F4')
            os.write(guest.master, b'\t')
            shown('Conversations')
            os.write(guest.master, b'\r')
            shown('NARROW_COMPOSER', absent='Conversations')
            os.write(guest.master, b'\x02')
            shown('Conversations')
            os.write(guest.master, b'\x02')
            shown('NARROW_COMPOSER', absent='Conversations')
            # A paste must not toggle panels or interpret control shortcuts.
            os.write(guest.master, b'\x1b[200~\x02PASTED\x1b[201~')
            shown('NARROW_COMPOSERPASTED', absent='Conversations')
            os.write(guest.master, b'\x04'); guest.read(guest.prompt, 30); active = False
            print(target + ': panel toggle, saved visibility, temporary chooser and 40-column composer passed', flush=True)
        finally:
            if active:
                os.write(guest.master, b'\x04'); guest.read(guest.prompt, 30)
            guest.close(); peer.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('native', 'ripple'), default='native')
    parser.add_argument('--ui-only', action='store_true', help='check only terminal layout, using a seeded conversation')
    args = parser.parse_args()
    run(args.target, args.ui_only)
