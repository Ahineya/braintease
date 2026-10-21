#!/usr/bin/env python3
"""End-to-end runtime checks. Guests always use newly constructed disposable disks."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from check import corpus

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
FIB = '280571172992510140037611932413038677189525n'


def notebook_script(name):
    book = json.loads(next((PORT / 'examples/notebooks').glob(name + '*')).read_text())
    return '\n'.join(c['content'].replace(':trace ', '') for c in book['cells']
                     if c['type'] == 'code') + '\n:quit\n'


def files():
    entries = [dict(name='/cases', path=str(corpus())),
               dict(name='/root/.keep', text=''), dict(name='/input', text='Alice\nx'),
               dict(name='/fib.repl', text=notebook_script('BigInt').replace('fib(200)','fib(20)'))]
    for folder, destination in [('library', '/usr/share/syma/library'),
                                ('examples', '/examples'), ('tests/programs', '/tests')]:
        for p in (PORT / folder).rglob('*.syma'):
            entries.append(dict(name=destination + '/' + str(p.relative_to(PORT / folder)), path=str(p)))
    for entry in entries:
        entry['mode'] = 0o100644
    return entries


def require(text, *markers):
    for marker in markers:
        if marker not in text:
            raise AssertionError('Missing ' + repr(marker) + '\n' + text[-6000:])


def host(binary, directory):
    env = os.environ | {'HOME': str(directory), 'SYMA_PATH': str(PORT / 'library')}

    def run(*args, source=None, expected=0):
        result = subprocess.run([str(binary), *map(str, args)], input=source, capture_output=True,
                                text=True, env=env, timeout=90)
        (directory / 'host.log').open('a').write(result.stdout + result.stderr)
        if result.returncode != expected:
            raise AssertionError(f'{args}: status {result.returncode}\n{result.stdout}\n{result.stderr}')
        return result.stdout

    require(run('--check', corpus()), 'checks='+str(len(corpus().read_text().splitlines()))+' failures=0')
    bad = directory / 'negative.syma'
    bad.write_text('{Case {Rules} 1 10 False False Result 2 False}\n')
    require(run('--check', bad, expected=1), 'failures=1')
    for source, answer in [('{Add 1 2}', '3'), ('{StrLen "😀"}', '2')]:
        require(run('eval', source), answer)
    for name in ['print-demo', 'experiments/bf']:
        text = run('run', PORT / 'examples' / (name + '.syma'))
        require(text, 'Printed message: Hello, Syma Print!' if name == 'print-demo' else 'Hello, World!')
    for name in ['BigInt', 'Optimizing Brainfuck', 'Cancellation', 'Unary', 'Archiver', 'Maze', 'FP']:
        text = run('repl', source=notebook_script(name))
        if name == 'BigInt': require(text, FIB)
    require(run('run', PORT / 'examples/input-demo.syma', source='Alice\nx'), 'Hello Alice!', 'You pressed: x')
    for name in ['io', 'storage-set', 'storage-get']:
        text = run('run', PORT / 'tests/programs' / (name + '.syma'))
        if name == 'io': require(text, 'IO_OK')
        if name == 'storage-get': require(text, 'STORAGE_OK')
    # A timer must fire while ReadLine is waiting, and partially typed input
    # must survive the normalization/effect round trip.
    process = subprocess.Popen([str(binary), 'run', str(PORT / 'tests/programs/timer.syma')],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               env=env)
    process.stdin.write(b'Al'); process.stdin.flush()
    require(process.stdout.readline().decode(), 'TIMER_OK')
    process.stdin.write(b'ice\n'); process.stdin.flush()
    stdout, stderr = process.communicate(timeout=15)
    require(stdout.decode(), 'INPUT_OK')
    assert process.returncode == 0, stderr.decode()
    # An input error must leave the REPL alive with the prior rule intact.
    require(run('repl', source=':rule inc Inc(x_) -> Add(x_ 1)\n)\nInc(4)\n:quit\n', expected=1), '5')


def guest(target, binary, directory):
    sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
    from guest import Guest
    # The symbolic decimal-arithmetic notebook is deliberately interpreted by
    # Syma too. RVM can take much longer than hosted execution for this workload.
    vm = Guest(directory, target, {'syma': binary}, files(), blocks=65536, timeout=2400)
    try:
        def shell(command, *markers, seconds=1800):
            text = vm.shell(command + '; echo SYMA_STATUS:$?', seconds)
            require(text, 'SYMA_STATUS:0', *markers)
            return text
        if os.environ.get('SYMA_TEST_PROGRESS'):
            shell('SYMA_TEST_PROGRESS=1; export SYMA_TEST_PROGRESS')
        shell('HOME=/root; export HOME; syma --check /cases', 'checks='+str(len(corpus().read_text().splitlines()))+' failures=0')
        shell('syma run /examples/print-demo.syma', 'Printed message: Hello, Syma Print!', 'Second message printed.')
        shell('syma run /examples/experiments/bf.syma', 'Hello, World!')
        shell('syma repl < /fib.repl', '6765n', seconds=600)
        shell('syma run /examples/input-demo.syma < /input', 'Hello Alice!', 'You pressed: x')
        shell('syma run /tests/io.syma', 'IO_OK')
        shell('syma run /tests/storage-set.syma')
        shell('syma run /tests/storage-get.syma', 'STORAGE_OK')
        os.write(vm.master, b'syma run /tests/timer.syma; echo SYMA_STATUS:$?\r')
        # Canonical guest terminal queues this prefix until the newline arrives.
        os.write(vm.master, b'Al')
        vm.read(b'TIMER_OK', 60)
        os.write(vm.master, b'ice\r')
        require(vm.read(vm.prompt, 60), 'INPUT_OK', 'SYMA_STATUS:0')
    finally:
        vm.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=['host', 'native', 'ripple'])
    parser.add_argument('--binary', type=Path)
    args = parser.parse_args()
    build = PORT / 'build' / {'host': 'host', 'native': 'fuse-x', 'ripple': 'fuzix'}[args.target]
    binary = (args.binary or build / {'host': 'syma', 'native': 'syma.fxe', 'ripple': 'syma.rxe'}[args.target]).resolve()
    directory = Path(tempfile.mkdtemp(prefix='test-' + args.target + '-', dir=PORT / 'build'))
    start = time.monotonic()
    if args.target == 'host': host(binary, directory)
    else: guest(args.target, binary, directory)
    result = dict(target=args.target, status='passed', seconds=round(time.monotonic() - start, 3),
                  binary=str(binary), logs=str(directory))
    (PORT / 'build' / ('result-' + args.target + '.json')).write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
