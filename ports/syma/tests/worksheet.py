#!/usr/bin/env python3
"""Worksheet and embedding checks; guests use explicit-manifest disposable disks."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from check import show, rules

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
DEMO = PORT / 'examples/worksheets/objects-and-math.symws'


def document(cells, limit=100000):
    return '{Worksheet V1 ' + str(limit) + '\n' + ''.join(
        '{Cell ' + kind + ' ' + json.dumps(source, ensure_ascii=False) + ' ""}\n'
        for kind, source in cells) + '}\n'


def oracle_cases():
    lines = []
    for line in (PORT / 'tests/worksheet-oracle.jsonl').read_text().splitlines():
        c = json.loads(line)
        lines.append('{Case ' + ' '.join([rules(c['rules']), show(c['expr']), '100000',
            'False', 'False', 'Result', show(c['expected']), 'False', show(c['universe'])]) + '}')
    return '\n'.join(lines) + '\n'


def require(text, *markers):
    for marker in markers:
        assert marker in text, ('missing ' + repr(marker), text[-6000:])


def embed_binary(target, binary, directory):
    spec = importlib.util.spec_from_file_location('syma_build', PORT / 'build.py')
    build = importlib.util.module_from_spec(spec); spec.loader.exec_module(build)
    if target == 'host':
        sanitized = 'sanitize' in binary.name
        archive = binary.parent / ('libsyma-sanitize.a' if sanitized else 'libsyma.a')
        out = directory / 'embed-check'
        sdk = ['-isysroot', subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()] if sys.platform == 'darwin' else []
        build.run(['/usr/bin/clang' if sys.platform == 'darwin' else 'cc', *sdk,
                   *(['-fsanitize=undefined'] if sanitized else []), '-std=c99',
                   '-I', PORT, PORT / 'tests/embed.c', archive, '-lm', '-o', out])
        return out
    sys.path.insert(0, str(ROOT / 'ports/quickjs'))
    from adapt import prepare
    work = PORT / 'build/embed-check' / target
    work.mkdir(parents=True, exist_ok=True)
    support = prepare(work / 'support')
    inputs = [PORT / (name + '.c') for name in build.SOURCES if name not in ('main', 'worksheet')]
    inputs += [support / (name + '.c') for name in build.SUPPORT]
    inputs += [PORT / 'tests/embed.c']
    if target == 'native':
        out = work / 'embed-check.fxe'
        build.run([sys.executable, ROOT / 'fuse-x/cc.py', *inputs, '-I', PORT, '-I', support,
                   '-D', '_BSD_SOURCE=1', '--float-format', '--static', '-o', out])
        return out
    return build.riplix(work, support, inputs, 4)


def fixtures(base):
    edit = ':run\n:edit 1\n{Define $value 41}\n.\n:show 2\n:run\n:move 2 1\n:list\n:move 1 2\n:remove 3\n:limit 5000\n:add text\nПривіт — a note\n..\n.\n:add code\n{StrLen "' + 'a' * 2200 + '"}\n.\n:run\n:save ' + base + '/saved.symws\n:quit\n'
    # Distinct source paths avoid depending on any preinstalled user's files.
    return {
        base + '/demo.symws': DEMO.read_text(),
        base + '/edit.symws': document([('Code', '{Define $value 40}'),
            ('Code', '{Add $value 2}'), ('Text', 'remove me')]),
        base + '/bad.symws': '{Worksheet V1 10 {Cell Code 42 ""}}',
        base + '/edit.commands': edit,
        base + '/reload.commands': ':list\n:show\n:run\n:quit\n',
        base + '/bad.commands': ':open ' + base + '/bad.symws\n:show 1\n:limit 0\n:run\n:quit\n',
        base + '/oracle.cases': oracle_cases(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target', choices=['host', 'native', 'ripple'])
    parser.add_argument('--binary', type=Path)
    a = parser.parse_args()
    folder, name = {'host': ('host', 'syma'), 'native': ('fuse-x', 'syma.fxe'),
                    'ripple': ('fuzix', 'syma.rxe')}[a.target]
    binary = (a.binary or PORT / 'build' / folder / name).resolve()
    directory = Path(tempfile.mkdtemp(prefix='worksheet-' + a.target + '-', dir=PORT / 'build'))
    start = time.monotonic()
    embed = embed_binary(a.target, binary, directory)
    base = str(directory) if a.target == 'host' else '/tmp'
    data = fixtures(base)
    demo_markers = ['{Output "woof" true false true false}', '[9] => 33', '[10] => 29',
                    '[15] => {Together {Output', 'Math/Small/Product 3']
    edit_markers = ['[2] => 42', 'Previous result (needs update): 42', '[2] => 43', 'Worksheet saved', '2200']
    reload_markers = ['[Needs update]', 'Previous result (needs update):', 'Привіт', '[2] => 43', '2200']
    if a.target == 'host':
        for path, source in data.items(): Path(path).write_text(source)
        env = os.environ | {'HOME': str(directory), 'SYMA_PATH': str(PORT / 'library')}
        def run(label, command, source=None, expected=0):
            r = subprocess.run(command, input=source, text=True, capture_output=True,
                               env=env, timeout=90)
            (directory / (label + '.log')).write_text(r.stdout + r.stderr)
            assert r.returncode == expected, (label, r.returncode, r.stdout, r.stderr)
            return r.stdout + r.stderr
        require(run('embed', [embed]), 'EMBED checks=398 failures=0')
        require(run('demo', [binary, 'worksheet', base + '/demo.symws', '--run']), *demo_markers)
        require(run('oracle', [binary, '--check', base + '/oracle.cases']), 'checks=8 failures=0')
        require(run('edit', [binary, 'worksheet', base + '/edit.symws'], data[base + '/edit.commands']), *edit_markers)
        saved = Path(base + '/saved.symws').read_text()
        assert '{Worksheet V1 5000' in saved and '\\u000a.\\u000a' in saved
        assert Path(base + '/saved.symws').stat().st_mode & 0o777 == 0o600
        require(run('reload', [binary, 'worksheet', base + '/saved.symws'], data[base + '/reload.commands']), *reload_markers)
        require(run('errors', [binary, 'worksheet', base + '/edit.symws'], data[base + '/bad.commands'], expected=1),
                'Cell needs Code|Text', '{Define $value 40}', '[2] => 42')
        run('bad-start', [binary, 'worksheet', base + '/bad.symws', '--run'], expected=1)
        # Mathematical checks across coefficients, powers and positive/negative evaluation points.
        cells = [('Code', '{Import Math/Small open}')]
        expected = []
        for degree in range(1, 7):
            for point in [-2, 0, 3]:
                cells.append(('Code', '{Substitute {Differentiate {Product 3 {Power x ' + str(degree) + '}} x} x ' + str(point) + '}'))
                expected.append(3 * degree * point ** (degree - 1))
        polynomial = directory / 'polynomials.symws'; polynomial.write_text(document(cells))
        text = run('polynomials', [binary, 'worksheet', polynomial, '--run'])
        for i, value in enumerate(expected, 2): require(text, '[' + str(i) + '] => ' + str(value) + '\n')
    else:
        sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
        from guest import Guest
        entries = [dict(name=p, text=s, mode=0o100644) for p, s in data.items()]
        for p in (PORT / 'library').glob('*.syma'):
            entries.append(dict(name='/usr/share/syma/library/' + p.name, path=str(p), mode=0o100644))
        vm = Guest(directory, a.target, {'syma': binary, 'embed-check': embed}, entries,
                   blocks=65536, timeout=1800)
        try:
            def shell(command, *markers, expected=0):
                text = vm.shell(command + '; echo SHEET_STATUS:$?', 900)
                require(text, 'SHEET_STATUS:' + str(expected), *markers)
            shell('embed-check', 'EMBED checks=398 failures=0')
            shell('syma worksheet /tmp/demo.symws --run', *demo_markers)
            shell('syma --check /tmp/oracle.cases', 'checks=8 failures=0')
            shell('syma worksheet /tmp/edit.symws < /tmp/edit.commands', *edit_markers)
            shell('syma worksheet /tmp/saved.symws < /tmp/reload.commands', *reload_markers)
            shell('syma worksheet /tmp/edit.symws < /tmp/bad.commands', 'Cell needs Code|Text',
                  '{Define $value 40}', '[2] => 42', expected=1)
            shell('syma worksheet /tmp/bad.symws --run', expected=1)
            # Real canonical tty: cell editor, discard protection and a saved restart.
            os.write(vm.master, b'syma worksheet /tmp/saved.symws --line\r')
            vm.read(b'sheet> ', 30)
            os.write(vm.master, b':add code\r')
            vm.read(b'Enter cell source, then . on its own line:', 30)
            os.write(vm.master, b'{Add 200 23}\r.\r')
            vm.read(b'sheet*> ', 30)
            os.write(vm.master, b':quit\r')
            vm.read(b'Discard unsaved changes? [y/N] ', 30)
            os.write(vm.master, b'n\r')
            vm.read(b'sheet*> ', 30)
            os.write(vm.master, b':save /tmp/tty.symws\r')
            vm.read(b'sheet> ', 30)
            os.write(vm.master, b':run\r')
            vm.read(b'[5] => 223', 120)
            vm.read(b'sheet*> ', 30)
            os.write(vm.master, b':save\r')
            vm.read(b'sheet> ', 30)
            os.write(vm.master, b':quit\r')
            vm.read(vm.prompt, 30)

        finally:
            vm.close()
    report = dict(target=a.target, status='passed', seconds=round(time.monotonic() - start, 3),
                  binary=str(binary), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  embedding_checks=398, oracle_cases=8, logs=str(directory))
    (PORT / 'build' / ('worksheet-' + a.target + '-result.json')).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__': main()
