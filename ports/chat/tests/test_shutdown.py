#!/usr/bin/env python3
"""Reproduce a reported Tcl tool call and distinguish host shutdown from tool exit.

Runs only temporary Fuse-X disks with fake credentials and a local HTTPS peer.
The sole host termination targets the test's own subprocess.
"""
import json
import os
import tempfile
import time
from pathlib import Path
from test_guest import ROOT, HOST, Service, Guest, make, evt

PROBE = '''puts [expr {255 & 255}]
puts [format %c 65]
puts [string index abc 1]
set a(0) 42
puts $a(0)
puts [llength {a b}]
puts $argv
set f [open /tmp/bf-probe.tcl r]
set s [read $f]
close $f
puts [string length $s]
'''


class Shutdown(Service):
    def completion(self, request):
        items = request['input']
        at = max(i for i, item in enumerate(items) if item.get('role') == 'user')
        prompt = items[at]['content']
        outputs = [item for item in items[at + 1:] if item.get('type') == 'function_call_output']
        if outputs:
            result = json.loads(outputs[-1]['output'])
            assert prompt == 'probe' and not result['interrupted'], result
            self.result = result
            message = dict(type='message', role='assistant', content=[dict(type='output_text', text='PROBE_DONE')])
            return (evt('response.output_text.delta', delta='PROBE_DONE') +
                    evt('response.completed', response=dict(status='completed', output=[message]))), prompt
        command = '/bin/tclsh /tmp/bf-probe.tcl' if prompt == 'probe' else 'sleep 30'
        item = dict(type='function_call', call_id='call_' + prompt, name='run_command',
                    arguments=json.dumps(dict(command=command, workdir='/', timeout_seconds=10 if prompt == 'probe' else 60)))
        return evt('response.completed', response=dict(status='completed', output=[item])), prompt


def main():
    build = ROOT / 'fuse-x/build'
    with tempfile.TemporaryDirectory(prefix='chat-shutdown-') as temporary:
        work = Path(temporary)
        certs = make(work / 'certs')
        peer = Shutdown(work / 'nic', certs)
        account = dict(issuer='https://auth.openai.com', subject='fixture-user', client_id='oaiapp_fixture',
                       ext_agent_host_id=HOST, token_type='Bearer', access_token='fixture-expired',
                       refresh_token='fixture-refresh', expires_at=1,
                       scopes=['resource.invoke', 'chatgpt.tokens.use.direct'])
        files = [dict(name='/etc/ssl/cert.pem', path=str(certs / 'ca.pem')),
                 dict(name='/chat/host-id', text=HOST, mode=0o100600),
                 dict(name='/credentials.json', text=json.dumps(account), mode=0o100600),
                 dict(name='/tmp/bf-probe.tcl', text=PROBE)]
        apps = {name: build / f'apps/{name}.fxe' for name in ('chat', 'chmod', 'tclsh', 'sleep')}
        guest = Guest(work, 'native', apps, files, peer, blocks=24576, disk_sectors=131072)
        try:
            guest.shell('chmod 700 /chat')
            env = 'CHAT_HOME=/chat CHAT_API_ORIGIN=https://tls.test CHAT_AUTH_ORIGIN=https://tls.test '
            assert 'IMPORT:0' in guest.shell(env + 'chat --import /credentials.json; echo IMPORT:$?')
            result = guest.shell(env + 'chat --workspace / --ask probe; echo CHAT:$?', 120)
            assert 'PROBE_DONE' in result and 'CHAT:0' in result, result
            assert 'STILL_ALIVE' in guest.shell('echo STILL_ALIVE'), result
            print('Tcl probe through real chat tools: guest survived; ' + json.dumps(peer.result), flush=True)
            os.write(guest.master, (env + 'chat --workspace / --ask host-stop\r').encode())
            guest.read(b'Running tool: run_command', 120)
            until = time.monotonic() + .5
            while time.monotonic() < until:
                guest.pump()
            guest.process.terminate()
            result = guest.read(b'filesystem synced; session stopped', 30)
            assert 'Halted.' in result, result
            assert guest.process.wait(timeout=10) == 0
            print('Host SIGTERM during a chat tool reproduces Halted + synced session stop; clean exit.', flush=True)
        finally:
            guest.close(); peer.close()


if __name__ == '__main__':
    main()
