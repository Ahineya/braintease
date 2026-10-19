#!/usr/bin/env python3
"""Run the actual CLI on a host reference or a disposable guest image."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('host', 'native', 'ripple'), default='host')
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/quickjs-cli-tests')
    parser.add_argument('--case', action='append', dest='selected',
                        help='Run an exact named case after a focused change')
    args = parser.parse_args()
    binary, build = args.binary.resolve(), args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    suffix = '-cli-selected-report.json' if args.selected else '-cli-report.json'
    report_path = build / (args.target + suffix)
    report_path.unlink(missing_ok=True)  # failed reruns must not leave old success
    fixtures = {
        'hello.js': "#!/bin/js\nprint('SCRIPT',scriptArgs[1],scriptArgs[2]);\nconsole.error('STDERR');",
        'stdin.js': "print('STDIN',70000+42);",
        'lib.mjs': 'export const answer=70042; export function double(x){return x*2;}',
        'main.mjs': "import {answer,double} from './lib.mjs'; print('MODULE',answer,double(21)); await Promise.resolve(); print('AWAIT',42);",
        'dynamic.js': "import('./lib.mjs').then(m=>print('DYNAMIC',m.answer));",
        'reject.mjs': "await Promise.reject(new Error('module rejected'));",
        'pending.mjs': 'await new Promise(()=>{});',
        'error.js': "function explode(){\nthrow new Error('expected failure');\n}\nexplode();",
        'bad.js': 'let = ;',
        'missing.mjs': "import './does-not-exist.mjs';",
        'nul.js': "print('before');\0print('after');",
        'bank.js': '//' + 'x' * 70000 + "\nprint('BANK',70042);",
        'formats.js': "function check(v,label){if(!v)throw new Error(label);}\n" + (PORT / 'tests/formats.js').read_text() + "\nprint('FORMATS',11);",
    }
    records, guest = [], None
    with tempfile.TemporaryDirectory(prefix='quickjs-cli-') as temporary:
        directory = Path(temporary)
        if args.target == 'host':
            prefix = str(directory)
            for name, source in fixtures.items(): (directory / name).write_text(source)
        else:
            sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
            from guest import Guest
            prefix = '/tmp'
            guest = Guest(directory, args.target, {'js': binary}, blocks=65536,
                          files=[dict(name='/tmp/' + name, text=source) for name, source in fixtures.items()])

        def invoke(label, argv, status=0, contains=(), stdin=None):
            if args.selected and label not in args.selected: return
            if guest:
                command = shlex.join(['js', *argv])
                if stdin is not None: command = 'cat /tmp/stdin.js | ' + command
                output = guest.shell(command + '; echo QJS_CLI_STATUS:$?', 60)
                # Ignore echoed input: a quoted source literal is not evidence
                # that the interpreter actually printed the expected value.
                output = output.split('\n', 1)[1]
                found = re.findall(r'^QJS_CLI_STATUS:(\d+)$', output, re.M)
                if len(found) != 1: raise AssertionError(label + ': missing CLI exit status\n' + output)
                code = int(found[0]); text = output.encode()
            else:
                process = subprocess.run([str(binary), *argv], input=stdin, capture_output=True, timeout=30)
                code, text = process.returncode, process.stdout + process.stderr
            if (code not in status if isinstance(status, tuple) else code != status) or any(value not in text for value in contains):
                raise AssertionError(f'{label}: expected exit {status}, got {code}: {text!r}')
            if isinstance(status, tuple):
                if code == 0 and b'BOOT\n' not in text:
                    raise AssertionError(label + ': did not execute source')
                if code == 1 and not any(message in text for message in (b'out of memory', b'cannot create context')):
                    raise AssertionError(label + ': unexpected failure: ' + repr(text))
            records.append(dict(case=label, status='passed', exit_code=code))
            print(args.target + ': ' + label, flush=True)

        try:
            invoke('version', ['--version'], contains=(b'QuickJS 2026-06-04',))
            invoke('help', ['--help'], contains=(b'Usage: js',))
            invoke('expression', ['-p', '70000+42'], contains=(b'70042\n',))
            invoke('Unicode output', ['-e', "print('Ж😀')"], contains=('Ж😀\n'.encode(),))
            invoke('embedded NUL output', ['-e', "print('A\\0B')"], contains=(b'A\0B\n',))
            invoke('console streams and arguments', [prefix + '/hello.js', 'one two', 'three'], contains=(b'SCRIPT one two three\n', b'STDERR\n'))
            invoke('stdin', ['-'], stdin=b"print('STDIN',70000+42);", contains=(b'STDIN 70042\n',))
            invoke('source crosses bank', [prefix + '/bank.js'], contains=(b'BANK 70042\n',))
            invoke('Date, diagnostics and Math', [prefix + '/formats.js'], contains=(b'FORMATS 11\n',))
            invoke('relative module and top-level await', [prefix + '/main.mjs'], contains=(b'MODULE 70042 42\n', b'AWAIT 42\n'))
            invoke('dynamic import', [prefix + '/dynamic.js'], contains=(b'DYNAMIC 70042\n',))
            invoke('Promise jobs', ['-e', "Promise.resolve(40).then(x=>print('PROMISE',x+2))"], contains=(b'PROMISE 42\n',))
            invoke('spaced arrow callback', ['-e', 'Promise.resolve(5).then(n => console.log(n))'],
                   contains=(b'5\n',))
            invoke('late handled rejection', ['-e', "const p=Promise.reject('late'); Promise.resolve().then(()=>p.catch(x=>print('HANDLED',x)))"], contains=(b'HANDLED late\n',))
            invoke('uncaught exception and stack', [prefix + '/error.js'], 1, (b'Error: expected failure\n', b'/error.js:2:'))
            invoke('syntax failure', [prefix + '/bad.js'], 1, (b'SyntaxError:', b'/bad.js:1:'))
            invoke('missing source', [prefix + '/absent.js'], 1, (b'js:', b'absent.js'))
            invoke('missing imported module', [prefix + '/missing.mjs'], 1, (b'ReferenceError:', b'does-not-exist.mjs'))
            invoke('unhandled Promise rejection', ['-e', "Promise.resolve().then(()=>{throw new Error('async failure')})"], 1, (b'Unhandled rejection: Error: async failure',))
            invoke('rejected top-level await', [prefix + '/reject.mjs'], 1, (b'Error: module rejected',))
            invoke('unsettled top-level await', [prefix + '/pending.mjs'], 1, (b'top-level await did not settle',))
            invoke('embedded NUL source rejected', [prefix + '/nul.js'], 1, (b'cannot read',))
            invoke('context memory limit', ['--memory', '1', '-e', '42'], 1, (b'cannot create context',))
            invoke('allocation failure', ['-e', 'new ArrayBuffer(4000000)'], 1, (b'out of memory',))
            for limit in (16384, 32768, 65536, 131072, 262144):
                invoke('setup under memory limit ' + str(limit), ['--memory', str(limit), '-e', "print('BOOT')"], (0, 1))
            invoke('console string conversion failure', ['-e', "print({toString(){throw new Error('conversion failure')}})"],
                   1, (b'Error: conversion failure',))
            invoke('recursive stack limit', ['-e', 'function f(){return f()} f()'], 1, (b'stack overflow',))
            invoke('interrupt budget', ['--max-polls', '3', '-e', 'while(true){}'], 1, (b'interrupted',))
            invoke('Promise job budget', ['--max-jobs', '8', '-e', 'function f(){Promise.resolve().then(f)} f()'], 1, (b'pending-job budget exceeded',))
            invoke('server mode timer', ['--server', '-e', 'setTimeout(()=>print("SERVER TIMER"),10)'], contains=(b'SERVER TIMER\n',))
            invoke('server mode keeps per-turn interrupt guard', ['--server', '--max-polls', '8', '-e', 'setTimeout(()=>{while(true){}},0)'], 1, (b'InternalError: interrupted',))
            invoke('server mode explicit wall budget', ['--server', '--max-time-ms', '40', '-e', 'setTimeout(()=>{},60000)'], 1, (b'event-loop time budget exceeded',))
            invoke('server mode explicit job budget', ['--server', '--max-jobs', '10', '-e', 'function loop(){return Promise.resolve().then(loop)}loop()'], 1, (b'pending-job budget exceeded',))
            invoke('server mode permits sustained jobs', ['--server', '-e', 'let n=0;function next(){if(++n===1200)print("SERVER JOBS");else Promise.resolve().then(next)}next()'], contains=(b'SERVER JOBS\n',))
            invoke('invalid option', ['--unknown'], 2, (b'Usage: js',))
            invoke('missing expression', ['-e'], 2, (b'Usage: js',))
            invoke('zero budget rejected', ['--max-polls', '0', '-e', '42'], 2, (b'Usage: js',))
            run_repl = not args.selected or 'REPL state and error recovery' in args.selected
            if guest and run_repl:
                os.write(guest.master, b'js -i; echo QJS_REPL_STATUS:$?\r')
                guest.read(b'js> ', 60)
                for line, expected in [('let x=40', None), ('x+2', '42\n'),
                                       ("throw new Error('repl failure')", 'Error: repl failure'),
                                       ('x+2', '42\n')]:
                    os.write(guest.master, line.encode() + b'\r')
                    output = guest.read(b'js> ', 60).split('\n', 1)[1]
                    if expected and expected not in output: raise AssertionError('REPL failed: ' + output)
                os.write(guest.master, b'.exit\r')
                output = guest.read(guest.prompt, 60)
                if 'QJS_REPL_STATUS:1' not in output: raise AssertionError('REPL lost error status: ' + output)
                records.append(dict(case='REPL state and error recovery', status='passed', exit_code=1))
            elif not guest:
                invoke('REPL state and error recovery', ['-i'], 1, (b'42\n', b'Error: repl failure'),
                       b"let x=40\nx+2\nthrow new Error('repl failure')\nx+2\n.exit\n")
                # Force a buffered stdout flush failure without filling a disk.
                import resource
                import signal
                def limited_output():
                    signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
                    resource.setrlimit(resource.RLIMIT_FSIZE, (0, 0))
                if not args.selected or 'buffered output flush failure' in args.selected:
                    with tempfile.TemporaryFile() as output:
                        failed = subprocess.run([str(binary), '-p', '42'], stdout=output,
                            stderr=subprocess.PIPE, preexec_fn=limited_output, timeout=10)
                    if failed.returncode != 1 or b'output write failed' not in failed.stderr:
                        raise AssertionError('Buffered result output failure was ignored')
                    records.append(dict(case='buffered output flush failure', status='passed', exit_code=1))
            if not args.selected or 'REPL arrow callbacks' in args.selected:
                lines = [('const a = 1;', ''), ('const b = 2;', ''),
                         ('console', '[object Object]\n'),
                         ('console.log("hello")', 'hello\n'),
                         ('console.log(a + b)', '3\n'),
                         ('Promise.resolve(5).then(n => console.log(n))', '5\n[object Promise]\n'),
                         ('Promise.resolve(5).then(n=>console.log(n))', '5\n[object Promise]\n'),
                         ('Promise.resolve(5).then((n) => console.log(n))', '5\n[object Promise]\n'),
                         ('Promise.resolve(5).then(n /* comment */ => console.log(n))', '5\n[object Promise]\n')]
                if guest:
                    os.write(guest.master, b'js -i; echo QJS_ARROW_STATUS:$?\r')
                    guest.read(b'js> ', 60)
                    for line, expected in lines:
                        os.write(guest.master, line.encode() + b'\r')
                        output = guest.read(b'js> ', 60).split('\n', 1)[1]
                        if output != expected + 'js> ':
                            raise AssertionError('REPL arrow callback failed: ' + repr(output))
                    os.write(guest.master, b'.exit\r')
                    output = guest.read(guest.prompt, 60)
                    if 'QJS_ARROW_STATUS:0' not in output:
                        raise AssertionError('REPL arrow callback exit failed: ' + output)
                    records.append(dict(case='REPL arrow callbacks', status='passed', exit_code=0))
                else:
                    process = subprocess.run([str(binary), '-i'],
                        input=('\n'.join(line for line, _ in lines) + '\n.exit\n').encode(),
                        capture_output=True, timeout=30)
                    expected = 'js> ' + ''.join(text + 'js> ' for _, text in lines)
                    if process.returncode or process.stderr or not process.stdout.decode().endswith(expected):
                        raise AssertionError('REPL arrow callbacks failed: ' + repr(process))
                    records.append(dict(case='REPL arrow callbacks', status='passed', exit_code=0))
            if args.selected and set(args.selected) != {record['case'] for record in records}:
                raise AssertionError('Unknown or unsupported selected CLI case')
        except BaseException:
            if guest and guest.process.poll() is None:
                os.write(guest.master, b'\x03')
                try: guest.read(guest.prompt, 10)
                except (AssertionError, OSError): pass
            raise
        finally:
            if guest:
                log_suffix = '-cli-selected-guest.log' if args.selected else '-cli-guest.log'
                (build / (args.target + log_suffix)).write_bytes(guest.log)
                guest.close()
        import hashlib
        report = dict(status='passed', target=args.target, cases=records,
                      executable_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                      filesystem_check='passed' if guest else 'not applicable')
        report_path.write_text(json.dumps(report, indent=2) + '\n')
        print(f'{args.target}: {len(records)} CLI cases passed', flush=True)


if __name__ == '__main__':
    main()
