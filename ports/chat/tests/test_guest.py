#!/usr/bin/env python3
"""Real C16 clients, guest TLS, independent HTTPS/SSE server, disposable disks."""
import argparse
import json
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import shutil
import time
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest
from fixtures import make
from peer import Services as TLS
from test_internet import Relay

HOST = 'urn:uuid:fc64362d-e773-49ad-9889-c8b90ad5d1be'
REASON = dict(type='reasoning', id='rs_fixture', encrypted_content='fixture-encrypted-reasoning')
MESSAGE = dict(type='message', id='msg_fixture', role='assistant', status='completed', phase='final_answer',
               content=[dict(type='output_text', text='Hello, Ж🙂!\nSecond line.', annotations=[])])


def evt(kind, **kw):
    return ('data: ' + json.dumps(dict(type=kind, **kw), ensure_ascii=False) + '\r\n\r\n').encode()


class Service(TLS):
    def __init__(self, path, certs):
        super().__init__(path, certs)
        self.calls = []
        self.refreshes = 0

    def completion(self, request):
        prompt = request['input'][-1]['content']
        payload = evt('response.created', response=dict(status='in_progress'))
        if prompt != 'empty':
            payload += evt('response.output_text.delta', delta='Hello, Ж🙂!\n')
            payload += evt('response.output_text.delta', delta='Second line.')
        if prompt not in ('failure', 'truncated', 'hang', 'sparse', 'hello-ui', 'reasononly', 'empty'):
            payload += evt('response.output_item.done', output_index=0, item=REASON)
            payload += evt('response.output_item.done', output_index=1, item=MESSAGE)
        if prompt == 'failure':
            payload += evt('response.failed', response=dict(status='failed'))
        elif prompt not in ('truncated', 'hang'):
            output = [] if prompt in ('hello-ui', 'sparse', 'doneonly', 'empty') else [REASON, MESSAGE]
            if prompt == 'reasononly':
                output = [REASON]
            payload += evt('response.completed', response=dict(status='completed', output=output))
        if prompt == 'cronly':
            payload = payload.replace(b'\r\n', b'\r')
        return payload, prompt

    def tls(self, stream):
        if not hasattr(stream, 'tls'):
            stream.incoming, stream.outgoing = ssl.MemoryBIO(), ssl.MemoryBIO()
            stream.tls = self.contexts[stream.local].wrap_bio(stream.incoming, stream.outgoing, server_side=True)
            stream.tls_at = 0
            stream.tls_ready = stream.responded = False
            stream.request = bytearray()
        fresh = bytes(stream.received[stream.tls_at:])
        stream.tls_at = len(stream.received)
        if fresh:
            stream.incoming.write(fresh)
        try:
            if not stream.tls_ready:
                stream.tls.do_handshake()
                stream.tls_ready = True
            while not stream.responded:
                stream.request.extend(stream.tls.read(16384))
                if b'\r\n\r\n' not in stream.request:
                    continue
                head, body = bytes(stream.request).split(b'\r\n\r\n', 1)
                lines = head.decode().split('\r\n')
                headers = dict(s.split(': ', 1) for s in lines[1:])
                if len(body) < int(headers.get('Content-Length', 0)):
                    continue
                path = lines[0].split()[1]
                stream.responded = True
                if path == '/api/accounts/oauth/token':
                    self.refreshes += 1
                    assert b'refresh_token=fixture-refresh' in body and b'client_id=oaiapp_fixture' in body, body
                    payload = json.dumps(dict(access_token='fixture-new', refresh_token='fixture-rotated',
                        token_type='Bearer', expires_in=3600, scope='resource.invoke chatgpt.tokens.use.direct')).encode()
                    response = b'HTTP/1.1 200 OK\r\nContent-Length: ' + str(len(payload)).encode() + b'\r\n\r\n' + payload
                elif path == '/v1/models':
                    response = b'HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n'
                else:
                    assert path == '/v1/responses', path
                    request = json.loads(body)
                    assert headers['Authorization'] == 'Bearer fixture-new', headers
                    assert request['model'] == 'gpt-6.1-sol' and request['reasoning']['effort'] == 'low'
                    assert request['stream'] and request['store'] is False
                    assert request['include'] == ['reasoning.encrypted_content']
                    assert not {'previous_response_id', 'conversation', 'max_output_tokens'} & request.keys()
                    self.calls.append(request)
                    payload, prompt = self.completion(request)
                    response = b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/event-stream\r\n\r\n'
                    # Split SSE lines, JSON escapes, UTF-8 codepoints and delimiters.
                    for at in range(0, len(payload), 7):
                        chunk = payload[at:at + 7]
                        response += f'{len(chunk):x}\r\n'.encode() + chunk + b'\r\n'
                    if prompt not in ('hang', 'badframing'):
                        response += b'0\r\n\r\n'
                stream.tls.write(response)
                if path == '/v1/responses' and prompt == 'hang':
                    return
                try:
                    stream.tls.unwrap()
                except ssl.SSLWantReadError:
                    pass
                stream.closing = True
        except ssl.SSLWantReadError:
            pass
        finally:
            while stream.outgoing.pending:
                stream.queue(stream.outgoing.read())


def run(target, internet=False):
    app = ROOT / ('fuse-x/build/apps/chat.fxe' if target == 'native' else 'build/fuzix/chat.rxe')
    with tempfile.TemporaryDirectory(prefix='fuse-chat-') as temp:
        directory = Path(temp)
        if shutil.disk_usage(directory).free < 160 * 1024 * 1024:
            raise RuntimeError('Chat guest tests need at least 160 MiB free; no image was created')
        if internet:
            peer = Relay(directory / 'nic', 'api.openai.com')
            files = [dict(name='/etc/ssl/cert.pem', path=str(ROOT / 'ports/tls/roots/cert.pem'))]
        else:
            certs = make(directory / 'certs')
            peer = Service(directory / 'nic', certs)
            account = dict(issuer='https://auth.openai.com', subject='fixture-user', client_id='oaiapp_fixture',
                ext_agent_host_id=HOST, token_type='Bearer', access_token='fixture-expired', refresh_token='fixture-refresh',
                expires_at=1, scopes=['resource.invoke', 'chatgpt.tokens.use.direct'])
            files = [dict(name='/etc/ssl/cert.pem', path=str(certs / 'ca.pem')),
                dict(name='/chat/host-id', text=HOST, mode=0o100600),
                dict(name='/credentials.json', text=json.dumps(account), mode=0o100600)]
        files.append(dict(name='/etc/termcap', text='ansi|vt100|xterm:co#80:li#24:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:ce=\\E[K:up=\\E[A:bc=^H:\n'))
        guest = Guest(directory, target, {'chat': app, 'chmod': ROOT / ('fuse-x/build/apps/chmod.fxe' if target == 'native' else 'build/fuzix/chmod.rxe')}, files, peer, blocks=24576, disk_sectors=131072)
        ui_active = False
        try:
            if internet:
                result = guest.shell('RIPPLE_TLS_PROFILE=1 chat --probe; echo PROBE:$?', 300)
                assert 'HTTPS_OK:' in result and 'PROBE:0' in result, result
                print(result)
            else:
                guest.shell('chmod 700 /chat')
                env = 'CHAT_HOME=/chat CHAT_API_ORIGIN=https://tls.test CHAT_AUTH_ORIGIN=https://tls.test '
                result = guest.shell(env + 'chat --list; echo EMPTY:$?')
                assert 'EMPTY:0' in result and 'New chat' not in result, result
                result = guest.shell(env + 'chat --import /credentials.json; echo IMPORT:$?')
                assert 'IMPORT:0' in result, result
                for prompt in ('hello', 'follow-up'):
                    result = guest.shell(env + f"chat --ask '{prompt}'; echo ASK:$?", 300)
                    assert 'ASK:0' in result and 'Reply saved' in result, result
                assert peer.refreshes == 1, peer.refreshes
                assert peer.calls[1]['input'] == [dict(role='user', content='hello'), REASON, MESSAGE, dict(role='user', content='follow-up')], peer.calls[1]
                result = guest.shell(env + 'chat --history')
                assert result.count('Second line.') == 2 and 'Ж🙂' in result, result
                result = guest.shell(env + 'chat --list')
                assert '1 hello' in result, result
                for prompt, expected in [('failure', 'did not complete'), ('truncated', 'without a complete response'), ('badframing', 'Truncated HTTPS')]:
                    result = guest.shell(env + f"chat --ask '{prompt}'; echo FAILED:$?", 300)
                    assert 'FAILED:1' in result and expected in result, result
                result = guest.shell(env + 'chat --history')
                assert result.count('Second line.') == 2, result
                result = guest.shell(env + "chat --ask 'cronly'; echo CRONLY:$?", 300)
                assert 'CRONLY:0' in result, result
                assert 'Second line.' in result, result
                for copies, prompt in enumerate(('doneonly', 'sparse', 'reasononly'), 4):
                    result = guest.shell(env + f"chat --ask '{prompt}'; echo RECOVERED:$?", 300)
                    assert 'RECOVERED:0' in result, result
                    result = guest.shell(env + 'chat --history')
                    assert result.count('Second line.') == copies, result
                # Individually completed reasoning/message objects are replayed intact.
                done_request = next(call for call in peer.calls if call['input'][-1]['content'] == 'sparse')
                assert done_request['input'][-4:] == [dict(role='user', content='doneonly'), REASON, MESSAGE,
                                                     dict(role='user', content='sparse')], done_request
                result = guest.shell(env + "chat --ask 'empty'; echo EMPTY_REPLY:$?", 300)
                assert 'EMPTY_REPLY:1' in result and 'without a reply' in result, result
                print(f'{target}: HTTPS streaming, sparse completion recovery, refresh rotation, complete replay, Unicode, history restart and failed/truncated responses passed', flush=True)
                # Assert actual rendered terminal contents, not only byte patterns.
                sys.path.insert(0, str(ROOT / 'rfl/tools'))
                from test_os import Screen
                screen = Screen()
                painted = len(guest.log)

                def shown(marker, timeout=60, absent=None):
                    nonlocal painted
                    end = time.monotonic() + timeout
                    while time.monotonic() < end:
                        guest.pump()
                        screen.feed(bytes(guest.log[painted:]))
                        painted = len(guest.log)
                        if marker in screen.text() and (absent is None or absent not in screen.text()):
                            return
                    raise AssertionError(f'UI missing {marker!r}\n' + screen.text())

                ui_active = True
                os.write(guest.master, (env + 'chat\r').encode())
                shown('Fuse Chat')
                os.write(guest.master, b'\x0ehello-ui\r')
                shown('Reply saved', 180, absent='Assistant (streaming)')
                assert 'Assistant' in screen.text() and 'Second line.' in screen.text(), screen.text()
                # A subsequent redraw must not erase with the header's blue style.
                os.write(guest.master, b'x')
                shown('x')
                assert screen.styles[10][40][1] == 0, screen.styles[10][40]
                os.write(guest.master, b'\x0eDraft\x0fline')
                shown('Draft')
                shown('line')
                os.write(guest.master, b'\x15hang\r')
                shown('Esc cancels', 180)
                os.write(guest.master, b'\x1b')
                shown('Cancelled', 60)
                assert peer.calls[-1]['input'] == [dict(role='user', content='hang')], peer.calls[-1]
                os.write(guest.master, b'\t\x1b[A\r')
                shown('Second line.')
                assert 'Assistant (streaming)' not in screen.text(), screen.text()
                os.write(guest.master, b'\x04')
                guest.read(guest.prompt, 30)
                ui_active = False
                locked = subprocess.run([sys.executable, str(ROOT / 'ports/chat/disk.py'), 'request',
                    str(guest.image), str(directory / 'locked.json'), '--state', '/chat'], capture_output=True)
                assert locked.returncode and b'in use' in locked.stderr, locked.stderr
                print(f'{target}: rendered UI, saved reply retention, background colors, multiline composer, send, cancellation, chat switching, clean exit and disk lock passed', flush=True)
        finally:
            # Leave raw-screen mode even when a rendered assertion fails.
            if ui_active:
                os.write(guest.master, b'\x04')
                guest.read(guest.prompt, 30)
            guest.close()
            peer.close()
        if not internet:
            transfer = directory / 'request.json'
            subprocess.run([sys.executable, str(ROOT / 'ports/chat/disk.py'), 'request', str(guest.image), str(transfer), '--state', '/chat'], check=True)
            assert json.loads(transfer.read_text())['ext_agent_host_id'] == HOST
            transfer.write_text(json.dumps(account))
            transfer.chmod(0o600)
            subprocess.run([sys.executable, str(ROOT / 'ports/chat/disk.py'), 'credentials', str(guest.image), str(transfer), '--state', '/chat'], check=True)
            print(f'{target}: stopped-disk sign-in transfer passed', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('native', 'ripple'), default='native')
    parser.add_argument('--internet', action='store_true')
    args = parser.parse_args()
    run(args.target, args.internet)
