#!/usr/bin/env python3
"""Actual guest tools over real HTTPS/SSE; an independent fixture plays the model.

Uses disposable disks and fake credentials. No OpenAI inference or Mac commands
are used by the guest. C is compiled in both guests and executed in FUZIX/RVM.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import tempfile
import time
from test_guest import ROOT, HOST, Service, Guest, make, evt


def call(index, name, arguments):
    return dict(type='function_call', id=f'fc_{index}', call_id=f'call_{index}',
                name=name, arguments=json.dumps(arguments))


class Agent(Service):
    def __init__(self, path, certs, target):
        super().__init__(path, certs)
        self.target = target
        command = 'cc main.c -o probe && '
        command += './probe; echo AGENT_EXIT:$?' if target == 'ripple' else 'echo AGENT_COMPILE_OK'
        self.steps = [
            ('list_directory', dict(path='.')),
            ('write_file', dict(path='main.c', content='int main(void) { return 23; }\n')),
            ('read_file', dict(path='main.c', offset=0, max_bytes=32768)),
            ('edit_file', dict(path='main.c', old_text='return 23', new_text='return 42')),
            ('run_command', dict(command=command, workdir='', timeout_seconds=120)),
            ('write_file', dict(path='very_long_script_filename.sh', content='echo AGENT_SCRIPT_OK; echo done > result.txt\n')),
            ('run_command', dict(command='sh very_long_script_filename.sh', workdir='', timeout_seconds=30)),
            ('run_command', dict(command='echo expected-failure >&2; exit 7', workdir='', timeout_seconds=10)),
            ('run_command', dict(command='sleep 10 & wait', workdir='', timeout_seconds=1)),
            ('read_file', dict(path='result.txt', offset=0, max_bytes=32768)),
        ]

    def completion(self, request):
        items = request['input']
        prompt = next(item['content'] for item in items if item.get('role') == 'user')
        outputs = [item for item in items if item.get('type') == 'function_call_output']
        if prompt == 'plain-tool':
            assert 'tools' not in request
            calls = [call(999, 'write_file', dict(path='/work/UNEXPECTED', content='BAD'))]
        else:
            assert request['parallel_tool_calls'] is False
            tools = {tool['name']: tool for tool in request['tools']}
            assert set(tools) == {'list_directory', 'read_file', 'write_file', 'edit_file', 'run_command'}
            assert all(tool['strict'] and tool['parameters']['additionalProperties'] is False for tool in tools.values())
            assert 'workspace /work' in request['instructions'], request['instructions']
            if prompt == 'agent-flow':
                for index, output in enumerate(outputs):
                    assert output['call_id'] == f'call_{index}', outputs
                    answer = json.loads(output['output'])
                    assert answer['ok'] == (index not in (7, 8)), (index, answer)
                    if index == 2: assert 'return 23' in answer['text'], answer
                    if index == 4:
                        expected = 'AGENT_EXIT:42' if self.target == 'ripple' else 'AGENT_COMPILE_OK'
                        assert expected in answer['text'], answer
                    if index == 6: assert 'AGENT_SCRIPT_OK' in answer['text'], answer
                    if index == 7: assert answer['exit_code'] == 7 and 'expected-failure' in answer['text'], answer
                    if index == 8: assert answer['timed_out'], answer
                    if index == 9: assert answer['text'] == 'done\n', answer
                if len(outputs) == len(self.steps):
                    return self.final('AGENT_FLOW_OK'), 'agent-flow'
                name, arguments = self.steps[len(outputs)]
                calls = [call(len(outputs), name, arguments)]
            elif prompt == 'agent-cancel':
                if outputs:
                    assert len(outputs) == 1 and json.loads(outputs[0]['output'])['ok'] is False, outputs
                    return self.final('AGENT_CANCEL_RESUMED_OK'), 'agent-cancel'
                calls = [call(800, 'run_command', dict(command='echo started >> started; sleep 30; echo BAD > TOO_LATE',
                                                       workdir='', timeout_seconds=60))]
            elif prompt == 'bad-call':
                calls = [call(900, 'write_file', dict(path='/work/INVALID_EFFECT', content='BAD')),
                         dict(type='function_call', name='write_file', arguments='{}')]
            else:
                raise AssertionError(prompt)
        index = len(outputs)
        reason = dict(type='reasoning', id=f'rs_{index}', encrypted_content=f'encrypted-{index}')
        payload = evt('response.created', response=dict(status='in_progress'))
        payload += evt('response.output_item.done', output_index=0, item=reason)
        payload += evt('response.function_call_arguments.delta', output_index=1, delta='{')
        for pos, item in enumerate(calls, 1):
            payload += evt('response.output_item.done', output_index=pos, item=item)
        # Exercise a final snapshot that omits all individually completed items.
        payload += evt('response.completed', response=dict(status='completed', output=[]))
        return payload, prompt

    def final(self, text):
        item = dict(type='message', id='final_message', role='assistant', status='completed',
                    content=[dict(type='output_text', text=text, annotations=[])])
        return evt('response.output_text.delta', delta=text) + evt('response.completed', response=dict(status='completed', output=[item]))


def run(target):
    build = ROOT / ('fuse-x/build' if target == 'native' else 'build/fuzix')
    package = build / 'compiler'
    app = build / ('apps/chat.fxe' if target == 'native' else 'chat.rxe')
    with tempfile.TemporaryDirectory(prefix='chat-agent-') as temporary:
        work = Path(temporary)
        certs = make(work / 'certs')
        peer = Agent(work / 'nic', certs, target)
        account = dict(issuer='https://auth.openai.com', subject='fixture-user', client_id='oaiapp_fixture',
                       ext_agent_host_id=HOST, token_type='Bearer', access_token='fixture-expired', refresh_token='fixture-refresh',
                       expires_at=1, scopes=['resource.invoke', 'chatgpt.tokens.use.direct'])
        files = [dict(name='/etc/ssl/cert.pem', path=str(certs / 'ca.pem')),
                 dict(name='/chat/host-id', text=HOST, mode=0o100600),
                 dict(name='/credentials.json', text=json.dumps(account), mode=0o100600),
                 dict(name='/work/README', text='Guest agent workspace\n'),
                 dict(name='/etc/termcap', text='ansi|vt100|xterm:co#80:li#24:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:ce=\\E[K:up=\\E[A:bc=^H:\n')]
        for row in json.loads((package / 'manifest.json').read_text()):
            if row['name'].startswith('/bin/') or row['name'] in ('/usr/lib/rcc/entry.rpo', '/usr/lib/rcc/libc.rpa'):
                files.append(dict(name=row['name'], path=str(package / row['file']), mode=row['mode']))
        ext = 'fxe' if target == 'native' else 'rxe'
        apps = {'chat': app}
        for name in ('chmod', 'sleep'):
            apps[name] = build / (f'apps/{name}.{ext}' if target == 'native' else f'{name}.{ext}')
        guest = Guest(work, target, apps, files, peer, blocks=65536, disk_sectors=393216)
        active = False
        try:
            guest.shell('chmod 700 /chat')
            env = 'CHAT_HOME=/chat CHAT_API_ORIGIN=https://tls.test CHAT_AUTH_ORIGIN=https://tls.test '
            assert 'IMPORT:0' in guest.shell(env + 'chat --import /credentials.json; echo IMPORT:$?')
            print(target + ': starting guest tool loop', flush=True)
            result = guest.shell(env + "chat --workspace /work --ask agent-flow; echo AGENT_STATUS:$?", 600)
            assert 'AGENT_STATUS:0' in result and 'AGENT_FLOW_OK' in result, result
            assert len(peer.calls) == len(peer.steps) + 1, len(peer.calls)
            history = guest.shell(env + 'chat --history')
            assert 'Tool: edit_file' in history and 'AGENT_FLOW_OK' in history, history
            assert 'return 42' in guest.shell('cat /work/main.c')
            print(target + ': all five tools, C compilation, script execution, errors, timeout and history passed', flush=True)

            # Open the existing workspace and create a new one using the UI command.
            sys.path.insert(0, str(ROOT / 'rfl/tools'))
            from test_os import Screen
            screen, painted = Screen(), len(guest.log)
            def shown(marker, timeout=120):
                nonlocal painted
                until = time.monotonic() + timeout
                while time.monotonic() < until:
                    guest.pump(); screen.feed(bytes(guest.log[painted:])); painted = len(guest.log)
                    if marker in screen.text(): return
                raise AssertionError(f'Missing {marker!r}\n' + screen.text())
            os.write(guest.master, (env + 'chat\r').encode()); active = True
            shown('Workspace: /work')
            os.write(guest.master, b'/workspace /work\r')
            shown('Workspace ready')
            os.write(guest.master, b'agent-cancel\r')
            shown('Running tool: run_command')
            until = time.monotonic() + 1
            while time.monotonic() < until: guest.pump()
            os.write(guest.master, b'\x1b')
            shown('Cancelled', 30)
            os.write(guest.master, b'\x04'); guest.read(guest.prompt, 30); active = False
            history = guest.shell(env + 'chat --history')
            assert 'Tool result' in history, history
            assert 'LATE:1' in guest.shell('cat /work/TOO_LATE; echo LATE:$?')
            before = guest.shell('cat /work/started').splitlines().count('started')
            # Retry continues after the durable result, without executing the call again.
            result = guest.shell(env + "chat --ask ''; echo RESUMED:$?", 300)
            assert 'AGENT_CANCEL_RESUMED_OK' in result and 'RESUMED:0' in result, result
            started = guest.shell('cat /work/started')
            assert started.splitlines().count('started') == before and before <= 1, started
            guest.shell(env + 'chat --workspace /work --list')
            # A genuinely plain chat must never dispatch an unsolicited tool.
            os.write(guest.master, (env + 'chat\r').encode()); active = True
            shown('Fuse Chat')
            os.write(guest.master, b'\x0eplain-tool\r')
            shown('no tools were executed')
            os.write(guest.master, b'\x04'); guest.read(guest.prompt, 30); active = False
            assert 'UNEXPECTED_STATUS:1' in guest.shell('cat /work/UNEXPECTED; echo UNEXPECTED_STATUS:$?')
            result = guest.shell(env + 'chat --workspace /work --ask bad-call; echo BAD_CALL_STATUS:$?', 300)
            assert 'Invalid function call fields' in result and 'BAD_CALL_STATUS:1' in result, result
            assert 'INVALID_EFFECT_STATUS:1' in guest.shell('cat /work/INVALID_EFFECT; echo INVALID_EFFECT_STATUS:$?')
            print(target + ': workspace UI/reopen, cancellation, safe retry and plain-chat isolation passed', flush=True)
        finally:
            if active:
                os.write(guest.master, b'\x04'); guest.read(guest.prompt, 30)
            guest.close(); peer.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('native', 'ripple'), default='native')
    run(parser.parse_args().target)
