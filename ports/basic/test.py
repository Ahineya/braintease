#!/usr/bin/env python3
"""Exercise the BASIC language, a real game, persistence and Rift on disposable disks."""
import argparse
import csv
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'rfl/tools'))
from test_os import Guest
from test_rift import make_image, F5


def run(command, **kw):
    return subprocess.run(list(map(str, command)), check=True, **kw)


def host(args, directory):
    executable = directory / 'basic'
    run([*shlex.split(args.cc), '-std=c99', '-Wall', '-Wextra', '-Werror',
         *[HERE / (name + '.c') for name in ('main', 'parse', 'value', 'run')],
         '-lm', '-o', executable])
    result = run([executable, HERE / 'tests/core.bas'], cwd=directory, capture_output=True, text=True)
    assert result.stdout == 'BRANCH_OK\nBASIC_CORE_OK\n', result
    failures = {
        'bad.bas': ('PRINT "BAD_SIDE_EFFECT"\nIF 1 THEN\nPRINT "oops"\n', 'unclosed block'),
        'unknown.bas': ('IF 0 THEN\nDRAW "R20"\nEND IF\n', 'unsupported BASIC'),
        'expr.bas': ('PRINT "BAD_SIDE_EFFECT"\nx = (3 + )\n', 'expected expression'),
        'args.bas': ('CALL x(1)\nEND\nSUB x()\nEND SUB\n', 'too many SUB'),
        'scope.bas': ('GOTO inside\nSUB x()\ninside:\nEND SUB\n', 'unknown label'),
        'divide.bas': ('x = 1 / 0\n', 'division by zero'),
        'types.bas': ('x$ = 2\n', 'assignment mismatch'),
        'quote.bas': ('PRINT "unclosed\n', 'unterminated string'),
        'boundary.bas': ('IF 1 THEN\nSUB x()\nEND SUB\nEND IF\n', 'SUB boundary'),
        'missing.bas': ('OPEN "missing/path" FOR INPUT AS #1\n', 'cannot open file'),
    }
    for name, (text, message) in failures.items():
        source = directory / name
        source.write_text(text)
        result = subprocess.run([str(executable), str(source)], cwd=directory, capture_output=True, text=True, timeout=10)
        assert result.returncode == 1 and message in result.stderr and 'BAD_SIDE_EFFECT' not in result.stdout, result
        assert str(source) + ':' in result.stderr, result.stderr
    source = directory / 'check.bas'
    source.write_text('OPEN "must-not-exist" FOR OUTPUT AS #1\nPRINT "BAD_SIDE_EFFECT"\nCLOSE #1\n')
    result = run([executable, '--check', source], cwd=directory, capture_output=True, text=True)
    assert 'syntax OK' in result.stdout and 'BAD_SIDE_EFFECT' not in result.stdout
    assert not (directory / 'must-not-exist').exists()
    # Validation of dead code must not advance the program's PRNG state.
    source.write_text('PRINT RND\nIF 0 THEN PRINT RND\n')
    first = run([executable, source], cwd=directory, capture_output=True).stdout
    source.write_text('PRINT RND\n')
    assert first == run([executable, source], cwd=directory, capture_output=True).stdout
    # A numeric INPUT retries without partially applying malformed records.
    source.write_text('INPUT "number", x\nPRINT x\nINPUT "name", n$\nPRINT n$\n')
    for ending in ['\n','\r','\r\n']:
        result = run([executable, source], cwd=directory, input=ending.join(['bad','42','Ada','']), capture_output=True, text=True)
        assert '?Redo from start' in result.stdout and ' 42 ' in result.stdout and 'Ada' in result.stdout
    run([executable, '--check', HERE / 'examples/granny.bas'], capture_output=True)
    steps = ['', 'N', 'Ada', '', '7', '3', '2', '2', '', '10', '4', '', '', '', 'Q', '5', '8', 'granny', 'Y']
    result = run([executable, HERE / 'examples/granny.bas'], cwd=directory,
                 input='\n'.join(steps)+'\n', capture_output=True, text=True, timeout=15)
    assert 'WELCOME TO BINGO MANIA!' in result.stdout and '+----------------------+' in result.stdout
    rows = list(csv.reader((directory / 'granny.sav').read_text().splitlines()))
    assert rows[0] == ['GRANNY1'] and rows[2][0] == 'Ada', rows
    assert rows[1][0] == '9' and rows[1][3] == '27' and rows[1][8] == '4', rows
    result = run([executable, HERE / 'examples/granny.bas'], cwd=directory,
                 input='\nL\ngranny\n1\n\n8\nagain\nY\n', capture_output=True, text=True, timeout=15)
    assert 'MONEY: 27 ' in result.stdout
    assert (directory/'granny.sav').read_bytes() == (directory/'again.sav').read_bytes()
    result = subprocess.run([str(executable), '--graphics=on', str(source)], capture_output=True)
    assert result.returncode == 2 and b'not implemented yet' in result.stderr
    print('host: language, diagnostics, text files and Granny Bingo play/save/load passed', flush=True)


def guest(args, system, directory):
    build = (args.fuzix_build if system == 'fuzix' else args.fuse_x_build).resolve()
    native = system == 'fuse-x'
    artifact = build / ('apps/rift.fxe' if native else 'rift.rxe')
    app = build / ('apps/basic.fxe' if native else 'basic.rxe')
    if not args.no_build:
        print(system + ': building BASIC and Rift', flush=True)
        if native:
            run([sys.executable, ROOT/'fuse-x/apps.py', 'basic', '--build', build])
            run([ROOT/'target/release/rfl', 'build', ROOT/'rfl/apps/rift/main.rfl', '--os', system,
                 '--build-dir', build, '-o', artifact])
        else:
            run(['make', '-C', ROOT/'ports/fuzix', 'BUILD='+str(build), '-j4', app, artifact])
    fixtures = {'/tmp/granny.bas': (HERE/'examples/granny.bas').read_text(),
                '/tmp/core.bas': (HERE/'tests/core.bas').read_text(),
                '/tmp/run.BAS': (HERE/'tests/runner.bas').read_text(),
                '/tmp/input.bas': 'INPUT "BASIC_INPUT", name$\nPRINT "INPUT_OK:"; name$\n',
                '/tmp/bad.bas': 'PRINT "BAD_SIDE_EFFECT"\nx = (3 + )\n'}
    image = make_image(system, build, artifact, directory,
                       extra_files=[{'name':'/bin/basic', 'source':str(app)}], extra_fixtures=fixtures)

    def boot():
        command = [build/'fuse-x', '--run', image] if native else [ROOT/'target/release/rvm', build/'bios.bin',
            '--disk', image, '--memory', '16384000', '--jit', '--net', 'off', '--audio', 'off',
            '--guest-ctrl-c', '--timeout', '240']
        g=Guest(command, system); g.raw(g.prompt); return g

    def stop(g, label):
        g.send(b'shutdown\r')
        g.raw(b'filesystem synced; session stopped' if native else b'Halted.', seconds=60)
        assert g.process.wait(timeout=20)==0
        (args.build.resolve()/f'{system}-{label}.log').write_bytes(g.log)
        g.close()

    g=boot()
    try:
        print(system+': checking language and Rift F5',flush=True)
        output=g.shell('cd /tmp; basic core.bas; echo BASIC_STATUS:$?')
        assert b'BASIC_CORE_OK' in output and b'FAIL:' not in output and b'BASIC_STATUS:0' in output, output
        output=g.shell('basic bad.bas; echo BAD_STATUS:$?')
        assert b'BAD_STATUS:1' in output and b'BAD_SIDE_EFFECT' not in output and b'bad.bas:2:' in output, output
        g.send(b'basic /tmp/input.bas\r');g.raw(b'BASIC_INPUT');g.send(b'Ada\r')
        g.raw(b'INPUT_OK:Ada');g.raw(g.prompt)
        g.send(b'rift /tmp/run.BAS\r');g.visible('NORMAL');g.visible('basic')
        # Change the file so F5 must save before invoking the detected runner.
        g.send(b'i \x1b');g.send(F5)
        g.raw(b'Rift Run:');g.raw(b'RIFT_BASIC_OK');g.raw(b'Program wait status: 0')
        g.raw(b'Press Enter to return to Rift')
        g.send(b'\r');g.visible('NORMAL');g.send(b':qa!\r');g.raw(g.prompt)
        print(system+': playing Granny Bingo and saving',flush=True)
        g.send(b'basic /tmp/granny.bas\r');g.raw(b'Press Enter');g.send(b'\r')
        g.raw(b'Load game or a new game(L/N)?');g.send(b'N\r')
        g.raw(b'whats your name?');g.send(b'Ada\r');g.raw(b'Press Enter');g.send(b'\r')
        g.raw(b'WHAT DO YOU WANT TO DO');g.send(b'7\r');g.raw(b'WHERE TO:');g.send(b'3\r')
        g.raw(b'10. TO EXIT');g.send(b'2\r');g.raw(b'HOW MANY?');g.send(b'2\r')
        g.raw(b'PRESS ENTER');g.send(b'\r');g.raw(b'10. TO EXIT');g.send(b'10\r')
        g.raw(b'WHERE TO:');g.send(b'4\r');g.raw(b'A ZERO WILL TAKE ITS PLACE');g.send(b'\r')
        g.raw(b'Enter for the next number, Q to go home');g.send(b'\r\r')
        g.raw(b'Enter for the next number, Q to go home');g.send(b'Q\r')
        g.raw(b'WHERE TO:');g.send(b'5\r');g.raw(b'WHAT DO YOU WANT TO DO');g.send(b'8\r')
        g.raw(b'What do you want to save it as');g.send(b'granny\r');g.raw(b'Exit(y/n)?');g.send(b'Y\r');g.raw(g.prompt)
        output=g.shell('cat /tmp/granny.sav')
        assert b'GRANNY1' in output and b'9,0,0,27,10,2,20,3,4,' in output and b'"Ada"' in output, output
        stop(g,'play')
    finally:
        if g.process.poll() is None: g.close()
    g=boot()
    try:
        print(system+': reloading after guest reboot',flush=True)
        g.send(b'cd /tmp; basic granny.bas\r');g.raw(b'Press Enter');g.send(b'\r')
        g.raw(b'Load game or a new game(L/N)?');g.send(b'L\r')
        g.raw(b'What file do you want to open');g.send(b'granny\r')
        g.raw(b'WHAT DO YOU WANT TO DO');g.send(b'1\r');g.raw(b'MONEY: 27 ')
        g.raw(b'Press Enter');g.send(b'\r');g.raw(b'WHAT DO YOU WANT TO DO');g.send(b'8\r')
        g.raw(b'What do you want to save it as');g.send(b'again\r');g.raw(b'Exit(y/n)?');g.send(b'Y\r');g.raw(g.prompt)
        stop(g,'reload')
    finally:
        if g.process.poll() is None: g.close()
    script = r'''
import {openImage} from './ports/fuzix/tools/image.mjs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {checkRoot} from './ports/fuzix/tools/fsck.mjs';
import {rootReader} from './ports/fuzix/tools/read-root.mjs';
const image=openImage(process.argv[1]);
const root=process.argv[2]==='fuse-x'?image:fatFile(image,'ROOTFS.IMG').data;
const checked=checkRoot(root);
if(checked.issues.length) throw Error(JSON.stringify(checked.issues));
const reader=rootReader(root);
if(reader.read('/tmp/run.BAS')[0]!==32) throw Error('Rift F5 did not save the edit');
const first=reader.read('/tmp/granny.sav'),second=reader.read('/tmp/again.sav');
if(first.length!==second.length || first.some((value,i)=>value!==second[i])) throw Error('save changed after reload');
if(!String.fromCharCode(...first).startsWith('"GRANNY1"\n9,0,0,27,')) throw Error('unexpected persisted game state');
console.log('BASIC test filesystem: clean');
'''
    run(['node','--input-type=module','-e',script,image,system],cwd=ROOT)
    print(system+': language, Rift F5 and Granny Bingo play/save/reboot/load passed',flush=True)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--system',choices=['host','both','fuse-x','fuzix'],default='both')
    p.add_argument('--no-build',action='store_true')
    p.add_argument('--cc',default='/usr/bin/clang -isysroot /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk' if sys.platform=='darwin' and Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk').exists() else 'cc')
    p.add_argument('--build',type=Path,default=ROOT/'build/basic')
    p.add_argument('--fuzix-build',type=Path,default=ROOT/'build/fuzix')
    p.add_argument('--fuse-x-build',type=Path,default=ROOT/'fuse-x/build')
    args=p.parse_args();args.build.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='basic-host-') as tmp: host(args,Path(tmp))
    for system in (['fuse-x','fuzix'] if args.system=='both' else [] if args.system=='host' else [args.system]):
        with tempfile.TemporaryDirectory(prefix='basic-'+system+'-') as tmp: guest(args,system,Path(tmp))


if __name__=='__main__': main()
