#!/usr/bin/env python3
"""Real guest terminal checks for tool folding, composer keys and @ completion."""
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
from unittest.mock import patch
from test_guest import ROOT, HOST, Service, Guest, make, evt

LARGE = 'REFERENCE_PREVIEW\n' + 'Reference text that belongs in the model context.\n' * 120 + 'TOOL_TAIL_SENTINEL'
PROMPT = 'Inspect @"docs dir/note.txt" and @alpha.txt '


class Interaction(Service):
    def completion(self, request):
        items = request['input']
        assert 'User mentions @path' in request['instructions']
        original = next(i for i in items if i.get('call_id') == 'original' and i['type'] == 'function_call_output')
        assert json.loads(original['output'])['text'] == LARGE
        last = max(i for i, item in enumerate(items) if item.get('role') == 'user')
        assert items[last]['content'] == PROMPT, items[last]
        outputs = [i for i in items[last + 1:] if i.get('type') == 'function_call_output']
        if not outputs:
            item = dict(type='function_call', call_id='mention-read', name='read_file',
                        arguments=json.dumps(dict(path='docs dir/note.txt', offset=0, max_bytes=32768)))
            return evt('response.completed', response=dict(status='completed', output=[item])), 'mention'
        assert json.loads(outputs[-1]['output'])['text'] == 'MENTIONED_FILE_CONTENT\n'
        message = dict(role='assistant', content='MENTION_REPLY_OK')
        return (evt('response.output_text.delta', delta='MENTION_REPLY_OK') +
                evt('response.completed', response=dict(status='completed', output=[message]))), 'mention'


def run(target, ui_only=False):
    build = ROOT / ('fuse-x/build' if target == 'native' else 'build/fuzix')
    ext = 'fxe' if target == 'native' else 'rxe'
    apps = {name: build / (f'apps/{name}.{ext}' if target == 'native' else f'{name}.{ext}') for name in ('chat', 'chmod')}
    history = [dict(role='user', content='Inspect a large reference'),
               dict(type='function_call', call_id='original', name='read_file',
                    arguments=json.dumps(dict(path='/work/reference.txt', offset=0, max_bytes=32768))),
               dict(type='function_call_output', call_id='original', output=json.dumps(dict(ok=True, text=LARGE))),
               dict(role='assistant', content='COMPACT_REPLY_VISIBLE')]
    with tempfile.TemporaryDirectory(prefix='chat-interaction-', dir='/tmp') as temporary:
        work = Path(temporary)
        certs = make(work / 'certs')
        peer = Interaction(work / 'nic', certs)
        account = dict(issuer='https://auth.openai.com', subject='fixture-user', client_id='oaiapp_fixture',
                       ext_agent_host_id=HOST, token_type='Bearer', access_token='fixture-expired', refresh_token='fixture-refresh',
                       expires_at=1, scopes=['resource.invoke', 'chatgpt.tokens.use.direct'])
        files = [dict(name='/etc/ssl/cert.pem', path=str(certs / 'ca.pem')),
                 dict(name='/chat/host-id', text=HOST, mode=0o100600),
                 dict(name='/credentials.json', text=json.dumps(account), mode=0o100600),
                 dict(name='/chat/c000001.jsonl', text=''.join(json.dumps(i) + '\n' for i in history), mode=0o100600),
                 dict(name='/chat/c000001.workspace', text='/work', mode=0o100600),
                 dict(name='/work/alpha.txt', text='ALPHA\n'),
                 dict(name='/work/docs dir/note.txt', text='MENTIONED_FILE_CONTENT\n'),
                 dict(name='/etc/termcap', text='ansi|vt100|xterm:co#80:li#24:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:ce=\\E[K:up=\\E[A:bc=^H:\n')]
        # Old VM runs may leave PID-named NIC directories in the shared TMPDIR.
        # A private temp root also keeps those artifacts inside this test's cleanup.
        with patch.dict(os.environ, TMPDIR=str(work)):
            guest = Guest(work, target, apps, files, peer, blocks=65536, disk_sectors=393216)
        active = False
        try:
            guest.shell('chmod 700 /chat')
            env = 'CHAT_HOME=/chat CHAT_API_ORIGIN=https://tls.test CHAT_AUTH_ORIGIN=https://tls.test '
            if not ui_only: assert 'IMPORT:0' in guest.shell(env + 'chat --import /credentials.json; echo IMPORT:$?')
            compact = guest.shell(env + 'chat --history')
            assert 'F4 expands tools' in compact and 'TOOL_TAIL_SENTINEL' not in compact, compact
            full = guest.shell(env + 'chat --history-full')
            assert 'TOOL_TAIL_SENTINEL' in full, full[-500:]
            sys.path.insert(0, str(ROOT / 'rfl/tools'))
            from test_os import Screen
            screen, painted = Screen(), len(guest.log)

            def shown(marker, absent=None, timeout=45):
                nonlocal painted
                end = time.monotonic() + timeout
                while time.monotonic() < end:
                    guest.pump(); screen.feed(bytes(guest.log[painted:])); painted = len(guest.log)
                    if marker in screen.text() and (absent is None or absent not in screen.text()): return
                raise AssertionError(f'Missing {marker!r}, excluding {absent!r}\n' + screen.text())

            def send(data): os.write(guest.master, data)

            send((env + 'chat\r').encode()); active = True
            shown('COMPACT_REPLY_VISIBLE', absent='TOOL_TAIL_SENTINEL')
            send(b'\x1b[<0;33;24M'); shown('TOOL_TAIL_SENTINEL')  # Click F4 tools.
            send(b'\x1b[<64;40;5M'); shown('Reference text', absent='TOOL_TAIL_SENTINEL')
            send(b'\x1b[<65;40;5M'); shown('TOOL_TAIL_SENTINEL')
            send(b'\x1b[<0;33;24M'); shown('COMPACT_REPLY_VISIBLE', absent='TOOL_TAIL_SENTINEL')
            # Composer clicks place its caret without sending a request.
            send(b'mouse click')
            shown('mouse click')
            send(b'\x1b[<0;8;20MX'); shown('mouseX click')
            send(b'\x15\x0b')
            # Sidebar mouse focus/open and footer panel controls.
            send(b'\x1b[<0;43;24M'); shown('Conversations')
            send(b'\x1b[<0;5;4M'); shown('COMPACT_REPLY_VISIBLE')
            send(b'alpha beta\x17gamma'); shown('alpha gamma', absent='alpha beta')
            send(b'\x01\x0b'); shown('COMPACT_REPLY_VISIBLE', absent='alpha gamma')
            send(b'\x19'); shown('alpha gamma')
            send(b'\x1bb\x1bddelta'); shown('alpha delta', absent='alpha gamma')
            send(b'\x15first\x0fsecond\x01\x0b\x19'); shown('second')
            send(b'\x01\x1b[AX\x05\x1b[B\x17done'); shown('Xfirst'); shown('done', absent='second')
            # Clear both lines using kill-before; then preserve controls during paste.
            send(b'\x15\x1b[200~PASTE\x17END\x1b[201~'); shown('PASTEEND', absent='done')
            send(b'\x15Inspect @doc'); shown('docs dir/'); shown('Files (@)')
            send(b'\x1b[<0;7;13M'); shown('note.txt')
            send(b'\x1b[<0;7;13M'); shown('@"docs dir/note.txt"', absent='Files (@)')
            send(b'and @al'); shown('alpha.txt'); shown('Files (@)')
            send(b'\r'); shown('@alpha.txt', absent='Files (@)')
            assert not peer.calls, 'Enter accepted a mention but also sent the message'
            if not ui_only:
                send(b'\r'); shown('MENTION_REPLY_OK', timeout=240)
                assert len(peer.calls) == 2
            else:
                send(b'\x15')
            # Completion remains usable in a narrow mux pane and Esc preserves draft.
            screen.resize(24, 40)
            fcntl.ioctl(guest.master, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 40, 0, 0))
            os.kill(guest.process.pid, signal.SIGWINCH)
            send(b'@al'); shown('Files (@)'); shown('alpha.txt')
            send(b'\x1b'); shown('@al', absent='Files (@)')
            send(b'\x17NARROW_OK'); shown('NARROW_OK')
            assert '@al' not in '\n'.join(''.join(row) for row in screen.lines[-5:-2])
            send(b'\x04'); guest.read(guest.prompt, 30); active = False
            print(target + ': tool folding/full history, Ctrl-W/K/U/Y, Alt-word editing, multiline movement, quoted @ paths and 40-column completion passed', flush=True)
        finally:
            if active:
                os.write(guest.master, b'\x04'); guest.read(guest.prompt, 30)
            guest.close(); peer.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('native', 'ripple'), default='native')
    parser.add_argument('--ui-only', action='store_true')
    args = parser.parse_args()
    run(args.target, args.ui_only)
