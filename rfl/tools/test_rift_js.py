#!/usr/bin/env python3
"""Drive JS highlighting, F8 detection, F5 run and error return on real guests."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
from test_os import Guest, ROOT
from test_rift import make_image, F5, F8


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--os', choices=('fuse-x', 'fuzix'), required=True)
    p.add_argument('--rift', type=Path, required=True)
    p.add_argument('--js', type=Path, required=True)
    p.add_argument('--build', type=Path, default=ROOT / 'build/quickjs-http-tests')
    a = p.parse_args()
    build = ROOT / ('fuse-x/build' if a.os == 'fuse-x' else 'build/fuzix')
    report = a.build.resolve() / (a.os + '-rift-js-report.json')
    report.parent.mkdir(parents=True, exist_ok=True); report.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix='rift-js-') as temp:
        directory = Path(temp)
        source = 'async function main($value) {\n    return /hello/.test($value);\n}\nconsole.log("RIFT_JS_OK", await main("hello"));\n'
        image = make_image(a.os, build, a.rift.resolve(), directory,
            applications=['sh', 'cat', 'echo'],
            extra_files=[dict(name='/bin/js', source=str(a.js.resolve()))],
            extra_fixtures={'/tmp/script.mjs': source,
                            '/tmp/broken.js': 'function broken( {\n',
                            '/tmp/app.server.mjs': 'import {sleep} from \'fuse:system\'; console.log(\'RIFT_SERVER_RUNNING\'); await sleep(86400000);\n'})
        command = [build / 'fuse-x', '--run', image] if a.os == 'fuse-x' else [
            ROOT / 'target/release/rvm', build / 'bios.bin', '--disk', image,
            '--memory', '16384000', '--net', 'off', '--jit', '--guest-ctrl-c', '--timeout', '300']
        guest = Guest(command, a.os)
        try:
            guest.raw(guest.prompt)
            guest.send(b'rift /tmp/script.mjs\r'); guest.visible('javascript'); guest.visible('F10 Menu')
            # A JS-only keyword: function is deliberately absent from C/RFL's table.
            guest.wait(lambda: guest.screen.styles[2][13][0] == 3, 'JavaScript function keyword')
            guest.send(F8); guest.visible('javascript: /bin/js'); guest.send(b'\r'); guest.visible('NORMAL')
            guest.at = len(guest.log); guest.send(F5)
            guest.raw(b'RIFT_JS_OK true', seconds=120); guest.raw(b'Press Enter to return to Rift')
            assert not guest.screen.alternate
            guest.send(b'\r'); guest.visible('Run finished; wait status 0'); assert guest.screen.alternate
            guest.send(b':'); guest.visible(': command'); guest.send(b'e /tmp/broken.js\r'); guest.visible('/tmp/broken.js')
            guest.at = len(guest.log); guest.send(F5); guest.raw(b'SyntaxError', seconds=120)
            guest.raw(b'Press Enter to return to Rift'); guest.send(b'\r'); guest.visible('Run finished; wait status')
            guest.send(b':'); guest.visible(': command'); guest.send(b'e /tmp/app.server.mjs\r'); guest.visible('/tmp/app.server.mjs')
            started = len(guest.log); guest.at = started; guest.send(F5); guest.raw(b'Rift Run:'); guest.raw(b'\nRIFT_SERVER_RUNNING', seconds=120)
            assert b'--server' in guest.log[started:]
            guest.send(b'\x03'); guest.raw(b'Press Enter to return to Rift'); guest.send(b'\r'); guest.visible('Run finished; wait status')
            guest.send(b':'); guest.visible(': command'); guest.send(b'qa!\r'); guest.raw(guest.prompt)
            guest.send(b'shutdown\r'); guest.process.wait(timeout=30); assert guest.process.returncode == 0
        finally:
            (a.build.resolve() / (a.os + '-rift-js-guest.log')).write_bytes(guest.log)
            guest.close()
        # Validate all temporary filesystem changes after orderly shutdown.
        if a.os == 'fuse-x':
            import sys
            sys.path.insert(0, str(ROOT / 'fuse-x'))
            from test_support import check_root
            check_root(image)
        else:
            import subprocess
            subprocess.run(['python3', str(ROOT / 'ports/fuzix/tools/fsck.py'), str(image)], check=True)
    report.write_text(json.dumps(dict(status='passed', system=a.os, cases=[
        'JS module detection', 'JS keyword highlighting', 'F8 PATH runner', 'F5 module execution',
        'syntax error report', 'F5 server mode and Ctrl-C', 'return to editor tabs', 'terminal restoration', 'clean filesystem'],
        rift_sha256=hashlib.sha256(a.rift.read_bytes()).hexdigest(),
        js_sha256=hashlib.sha256(a.js.read_bytes()).hexdigest()), indent=2) + '\n')
    print(a.os + ': Rift JavaScript workflow passed', flush=True)


if __name__ == '__main__': main()
