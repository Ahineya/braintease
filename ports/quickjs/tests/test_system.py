#!/usr/bin/env python3
"""Exercise real files, timers and child pipes on the host and disposable guests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]


def child_binary(target, build, compiler, runtime):
    if target == 'host':
        binary = build / 'system-child-host'
        command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-O2', str(PORT / 'tests/system-child.c'), '-o', str(binary)]
        if sys.platform == 'darwin' and os.environ.get('SDKROOT'):
            command += ['-isysroot', os.environ['SDKROOT']]
        subprocess.run(command, check=True)
        return binary
    binary = build / ('system-child.fxe' if target == 'native' else 'system-child.rxe')
    if target == 'native':
        subprocess.run([sys.executable, str(ROOT / 'fuse-x/cc.py'),
            str(PORT / 'tests/system-child.c'), '--compiler', str(compiler),
            '--build', str(runtime / 'sdk-native'), '-o', str(binary)], check=True)
    else:
        sdk = runtime / 'sdk-ripple'
        info = json.loads(subprocess.check_output(['make', '-s', '--no-print-directory', '-C',
            str(ROOT / 'ports/fuzix'), 'BUILD=' + str(sdk), 'RCC=' + str(compiler), 'rfl-sdk-info'], text=True))
        assembly, obj = build / 'system-child.asm', build / 'system-child.pobj'
        subprocess.run([str(compiler), 'compile', str(PORT / 'tests/system-child.c'), '--target', 'ripple',
            '-DRIPPLE_MMU=1', '-I', str(ROOT / 'ports/fuzix/ripple/include'),
            '-I', str(ROOT / 'ports/fuzix/upstream/Library/include'),
            '-I', str(ROOT / 'runtime/include'), '--banked-globals', '-o', str(assembly)], check=True,
            stdout=subprocess.DEVNULL, env=dict(os.environ, RUST_LOG='error'))
        assembler = ROOT / 'src/ripple-asm/target/release/rasm'
        subprocess.run([str(assembler), 'assemble', str(assembly), '-o', str(obj),
            '--bank-size', '64000', '--max-immediate', '65535'], check=True)
        subprocess.run([str(assembler.with_name('rlink')), info['entry'], str(obj), *info['libraries'],
            '-f', 'rxe', '--bank-size', '64000', '-o', str(binary)], check=True, stdout=subprocess.DEVNULL)
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('host', 'native', 'ripple'), default='host')
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--child', type=Path)
    parser.add_argument('--compiler', type=Path, help='defaults to the binary build directory\'s rcc-pinned, then target/release/rcc')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/quickjs-system-tests')
    args = parser.parse_args()
    binary, build = args.binary.resolve(), args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    report = build / (args.target + '-system-report.json')
    report.unlink(missing_ok=True)
    compiler = args.compiler or binary.parent / 'rcc-pinned'
    if not args.compiler and not compiler.exists(): compiler = ROOT / 'target/release/rcc'
    child = args.child.resolve() if args.child else child_binary(args.target, build, compiler.resolve(), binary.parent)
    records = []
    with tempfile.TemporaryDirectory(prefix='quickjs-system-') as temporary:
        directory = Path(temporary)
        prefix = str(directory) if args.target == 'host' else '/tmp/system'
        fixtures = {
            'sample.json': '{"message":"Привет 🌍"}',
            'unicode-source.txt': 'Καλημέρα! こんにちは! Привет 🌍\n',
            'nul.txt': 'A\0B', 'bank.txt': 'x' * 70000,
            'not-executable': '#!/bin/sh\necho forbidden\n',
            'system.mjs': (PORT / 'tests/system.mjs').read_text(),
            'demo.mjs': (PORT / 'examples/system-info.mjs').read_text(),
            'demo.json': (PORT / 'examples/system-sample/sample.json').read_text(),
            'notes.txt': (PORT / 'examples/system-sample/notes.txt').read_text(),
        }
        guest = None
        if args.target == 'host':
            for name, source in fixtures.items(): (directory / name).write_text(source)
            guest_child = str(child)
        else:
            sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
            from guest import Guest
            guest = Guest(directory, args.target, {'js': binary, 'qjs-child': child}, blocks=65536,
                files=[dict(name=prefix + '/' + name, text=source) for name, source in fixtures.items()])
            guest_child = '/bin/qjs-child'

        def invoke(label, arguments, status, expected):
            if guest:
                output = guest.shell(shlex.join(['js', *arguments]) + '; echo SYSTEM_STATUS:$?', 600)
                output = output.split('\n', 1)[1]
                import re
                statuses = re.findall(r'^SYSTEM_STATUS:(\d+)$', output, re.M)
                if statuses != [str(status)]: raise AssertionError(label + ': ' + output)
            else:
                process = subprocess.run([str(binary), *arguments], capture_output=True, timeout=120)
                output = (process.stdout + process.stderr).decode()
                if process.returncode != status: raise AssertionError(label + ': ' + output)
            if any(text not in output for text in expected): raise AssertionError(label + ': ' + output)
            records.append(dict(case=label, status='passed'))
            print(args.target + ': ' + label + '\n' + output[-400:], flush=True)

        try:
            invoke('system APIs and asynchronous work', ['--max-time-ms', '300000', prefix + '/system.mjs', prefix, guest_child],
                   0, ['SYSTEM CHECKS'])
            invoke('system utility demo', ['--max-time-ms', '300000', prefix + '/demo.mjs', prefix,
                prefix + '/demo.json', prefix + '/demo-report.json'], 0,
                ['JavaScript on Fuse-X / FUZIX', 'Привет, мир!', 'Saved ' + prefix + '/demo-report.json'])
            invoke('timer keeps script alive', ['-e', 'setTimeout(() => console.log("ALIVE"), 10)'], 0, ['ALIVE\n'])
            invoke('top-level await on timer', ['-m', '-e', 'import {sleep} from "fuse:system"; await sleep(10); console.log("AWAIT TIMER")'],
                   0, ['AWAIT TIMER\n'])
            invoke('timer exception', ['-e', 'setTimeout(() => { throw new Error("timer failed") }, 0)'], 1, ['Error: timer failed'])
            invoke('timer rejection', ['-e', 'setTimeout(() => Promise.reject(new Error("timer rejected")), 0)'], 1,
                   ['Unhandled rejection: Error: timer rejected'])
            invoke('result conversion schedules work', ['-p', '({toString(){setTimeout(()=>console.log("RESULT TIMER"),10);return "display"}})'],
                   0, ['RESULT TIMER\ndisplay\n'])
            invoke('result conversion rejects Promise', ['-p', '({toString(){Promise.reject("result rejection");return "display"}})'],
                   1, ['Unhandled rejection: result rejection'])
            invoke('event-loop time budget', ['--max-time-ms', '40', '-e', 'setTimeout(() => console.log("TOO LATE"), 60000)'],
                   1, ['event-loop time budget exceeded'])
            invoke('self-scheduling timer budget', ['--max-time-ms', '40', '-e', 'function again(){setTimeout(again,0)} again()'],
                   1, ['event-loop time budget exceeded'])
            invoke('cancel running child on budget', ['--max-time-ms', '3000', '-m', '-e',
                'import {run} from "fuse:system"; await run(' + json.dumps(guest_child) + ',["hang",' + json.dumps(prefix + '/budget.pid') + '])'],
                1, ['event-loop time budget exceeded'])
            probe = 'import {run} from "fuse:system"; const r=await run(' + json.dumps(guest_child) + ',["gone",' + json.dumps(prefix + '/budget.pid') + ']); console.log("REAPED",r.code)'
            invoke('budget child is gone', ['-m', '-e', probe], 0, ['REAPED 0'])
            interrupt_source = ('import {run} from "fuse:system"; '
                'setTimeout(()=>console.log("SIGNAL_"+"READY"),1000); '
                'await run(' + json.dumps(guest_child) + ',["hang",' + json.dumps(prefix + '/signal.pid') + ']);')
            if guest:
                # A fixture avoids the shell's 255-cell interactive line limit.
                # Construct its path with an ordinary guest redirection.
                guest.shell('echo ' + shlex.quote(interrupt_source) + ' > /tmp/interrupt.mjs', 60)
                os.write(guest.master, b'js /tmp/interrupt.mjs; echo INTERRUPT_STATUS:$?\r')
                guest.read(b'\nSIGNAL_READY', 60)
                os.write(guest.master, b'\x03')
                output = guest.read(guest.prompt, 60)
                if 'INTERRUPT_STATUS:130' not in output: raise AssertionError('SIGINT cleanup failed: ' + output)
            else:
                process = subprocess.Popen([str(binary), '-m', '-e', interrupt_source],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                import select
                if not select.select([process.stdout], [], [], 10)[0]:
                    process.kill(); process.wait(); raise AssertionError('Signal fixture did not start')
                if process.stdout.readline() != b'SIGNAL_READY\n':
                    process.kill(); process.wait(); raise AssertionError('Missing signal-ready marker')
                process.send_signal(signal.SIGINT)
                output, error = process.communicate(timeout=10)
                if process.returncode != 130: raise AssertionError('SIGINT cleanup failed: ' + repr(error))
            records.append(dict(case='interrupt running child', status='passed'))
            probe = probe.replace('/budget.pid', '/signal.pid')
            invoke('interrupted child is gone', ['-m', '-e', probe], 0, ['REAPED 0'])
        finally:
            if guest:
                (build / (args.target + '-system-guest.log')).write_bytes(guest.log)
                guest.close()
    report.write_text(json.dumps(dict(status='passed', target=args.target, cases=records,
        executable_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
        filesystem_check='passed' if guest else 'not applicable'), indent=2) + '\n')


if __name__ == '__main__':
    main()
