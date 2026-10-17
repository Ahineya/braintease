#!/usr/bin/env python3
"""Build the existing Ripple FUZIX applications for the hosted native ABI.

--all records every result in build/application-report.json; build success is
not a claim of runtime compatibility. No files in ports/fuzix are generated.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
PORT = HERE.parent / 'ports/fuzix'
NETWORK = set('ifconfig ping telnet telnetd htget dig echoping ntpdate host nslookup'.split())
ALIASES = dict(mv='cp', ln='cp', vi='levee', tcl='picol', tclsh='picol', sokoban='sok', nslookup='host')
# The first GX device is MMU/RVM-only; native window service is a later milestone.
RVM_ONLY = {'gterm'}


def names():
    text = (PORT / 'Makefile').read_text()
    return sorted(set(re.findall(r'^APPS\s*[:+]=\s*(.*)$', text, re.M)[0].split() +
                      [n for line in re.findall(r'^APPS\s*\+=\s*(.*)$', text, re.M) for n in line.split()] +
                      ['sh', 'mv', 'ln', 'vi', 'tcl', 'tclsh', 'sokoban', 'nslookup']) - RVM_ONLY)


def recipe(name):
    if name in RVM_ONLY:
        raise ValueError(f'{name} requires the GX graphics backend; the Fuse-X backend is not implemented yet')
    name = ALIASES.get(name, name)
    base = PORT / 'upstream/Applications'
    flags = ['-DUIDGID=0']
    if name == 'sh':
        directory = base / 'V7/cmd/sh'
        sources = [directory / (s + '.c') for s in
                   'args blok builtin cmd ctype error expand fault glob io macro main msg name print service setbrk stak string word xec'.split()]
        sources += [PORT / 'ripple/shell-input.c', PORT / 'ripple/shell-jobs.c']
        return sources, ['-DBUILD_FSH', '-I' + str(directory)]
    if name == 'levee':
        directory = base / 'levee'
        sources = [directory / (s + '.c') for s in
                   'blockio display editcor exec find globals insert main misc modify move ucsd undo unixcall wildargs'.split()]
        return sources, ['-DANSI=1', '-DVT52=0', '-DSIZE=30000', '-DRIPPLE', '-I' + str(directory)]
    if name == 'awk':
        sources = [PORT / 'awk' / (s + '.c') for s in
                   'awkgram.tab b main parse proctab tran lib run lex'.split()]
        return sources + [PORT / 'ripple/awk-support.c'], ['-I' + str(PORT / 'awk'), '--float-format']
    if name in 'find test expr make m4'.split():
        source = base / f'MWC/cmd/{name}.c'
    elif name in 'comm rev join split tsort tty nice time clear pr col diff diffh dc'.split():
        source = base / f'V7/cmd/{name}.c'
    elif name in ['cowsay', 'hamurabi', 'sok']:
        source = base / f'games/{name}.c'
    elif name == 'wump':
        source = base / 'V7/games/wump.c'
    elif name == 'picol':
        source = base / 'TCL/picol.c'
    elif name in 'ps top ptysh ifconfig ping telnet telnetd host bf'.split():
        source = PORT / f'ripple/{name}.c'
    elif name in 'htget dig echoping ntpdate'.split():
        source = base / f'netd/{name}.c'
    else:
        source = base / f'util/{name}.c'
    if name == 'cut': flags += ['-Dwarn=cut_warn']
    if name == 'grep': flags += ['-Dregerror=grep_regerror']
    if name == 'vile': flags += ['-DRIPPLE']
    return [source], flags


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('names', nargs='*')
    parser.add_argument('--all', action='store_true')
    parser.add_argument('--compiler', type=Path, default=HERE.parent / 'target/release/rcc')
    parser.add_argument('--build', type=Path, default=HERE / 'build')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    selected = names() if args.all else args.names
    if not selected: parser.error('choose applications or --all')
    build = args.build.resolve()
    (build / 'apps').mkdir(parents=True, exist_ok=True)
    (build / 'application-logs').mkdir(exist_ok=True)
    sdk = hashlib.sha256()
    sdk.update(str((args.compiler.resolve(), args.compiler.stat().st_mtime_ns)).encode())
    for path in sorted(set([HERE / 'cc.py', HERE / 'libc/bridge.c', PORT / 'ripple/userlib.c',
                            PORT / 'upstream/Kernel/include/userstructs.h'] +
                           list((PORT / 'ripple/libc').glob('*.c')) +
                           list((PORT / 'upstream/Library/libs').glob('*.c')) +
                           list((PORT / 'ripple').glob('*.h')) +
                           list((PORT / 'ripple/include').rglob('*.h')) +
                           list((PORT / 'upstream/Library/include').rglob('*.h')) +
                           list((HERE.parent / 'runtime/include').glob('*.h')))):
        sdk.update(str(path).encode()); sdk.update(path.read_bytes())
    sdk_fingerprint = sdk.digest()

    def build_one(name):
        sources, flags = recipe(name)
        compile_sources = sources
        if name == 'sh':
            # V7 represents "stdin selected" by decrementing a null char *.
            # RCC native checks GEP bounds, so form the sentinel explicitly.
            # Keep the imported source intact and adapt only this build copy.
            original = PORT / 'upstream/Applications/V7/cmd/sh/main.c'
            text = original.read_text()
            if text.count('comdiv--;') != 1:
                raise RuntimeError('Bourne shell stdin sentinel adaptation needs review')
            generated = build / 'application-sources/sh-main.c'
            generated.parent.mkdir(exist_ok=True)
            generated.write_text(text.replace('comdiv--;', 'comdiv = (char *)-1;'))
            compile_sources = [generated if s == original else s for s in sources]
        if ALIASES.get(name, name) == 'levee':
            # Saving via /tmp cannot rename atomically onto a host mount.
            # Keep the imported editor intact and place its native save file
            # beside the destination, as required across filesystem boundaries.
            original = PORT / 'upstream/Applications/levee/exec.c'
            text = original.read_text()
            old = 'char temporary[] = "/tmp/lvXXXXXX";'
            call = '    fd = mkstemp(temporary);'
            if text.count(old) != 1 or text.count(call) != 1:
                raise RuntimeError('Levee same-directory save adaptation needs review')
            text = text.replace(old, 'char temporary[96], *separator;\n    unsigned prefix;')
            text = text.replace(call,
                "    separator = strrchr(fname, '/');\n"
                '    prefix = separator ? (unsigned)(separator - fname + 1) : 0;\n'
                '    memcpy(temporary, fname, prefix);\n'
                '    strcpy(temporary + prefix, ".lvXXXXXX");\n' + call)
            generated = build / 'application-sources/levee-exec.c'
            generated.parent.mkdir(exist_ok=True)
            generated.write_text(text)
            compile_sources = [generated if s == original else s for s in sources]
        output = build / 'apps' / (name + '.fxe')
        log = build / 'application-logs' / (name + '.log')
        stamp = build / 'application-logs' / (name + '.json')
        digest = hashlib.sha256(sdk_fingerprint + str(flags).encode())
        for path in sorted(set(compile_sources + [h for s in sources for h in s.parent.glob('*.h')])):
            digest.update(str(path).encode()); digest.update(path.read_bytes())
        key = digest.hexdigest()
        if stamp.exists():
            previous = json.loads(stamp.read_text())
            if previous.get('fingerprint') == key and (previous['status'] == 'blocked' or output.exists()):
                print(f"{name}: {previous['status']} (cached)", flush=True)
                return previous
        canonical = ALIASES.get(name)
        if canonical:
            original = build / 'application-logs' / (canonical + '.json')
            if original.exists():
                previous = json.loads(original.read_text())
                if previous.get('fingerprint') == key and previous['status'] == 'built':
                    shutil.copy2(build / 'apps' / (canonical + '.fxe'), output)
                    previous.update(name=name, runtime='not tested', log=str(log), seconds=0)
                    log.write_text(f'Alias of {canonical}; identical sources and compile flags.\n')
                    stamp.write_text(json.dumps(previous, indent=2) + '\n')
                    print(f'{name}: built (alias of {canonical})', flush=True)
                    return previous
        start = time.monotonic()
        command = [sys.executable, str(HERE / 'cc.py'), *map(str, compile_sources), *flags,
                   '--compiler', str(args.compiler.resolve()), '--build', str(build), '-o', str(output)]
        with log.open('w') as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
        # Keep stale successes out of the install set after a failed rebuild.
        if result.returncode:
            output.unlink(missing_ok=True)
        status = 'built' if result.returncode == 0 else 'blocked'
        print(f'{name}: {status}', flush=True)
        record = dict(name=name, status=status, runtime='not tested', fingerprint=key,
                    requirements=['hosted networking'] if name in NETWORK else [],
                    sources=[str(s.relative_to(PORT)) for s in sources],
                    log=str(log), seconds=round(time.monotonic() - start, 2),
                    bytes=output.stat().st_size if output.exists() else None,
                    reason=log.read_text()[-3000:] if result.returncode else None)
        stamp.write_text(json.dumps(record, indent=2) + '\n')
        return record

    def compile_one(name):
        try:
            return build_one(name)
        except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
            # The port's application list can grow before a hosted recipe is
            # available. Keep auditing other programs and record that blocker.
            log = build / 'application-logs' / (name + '.log')
            log.write_text(str(error) + '\n')
            (build / 'apps' / (name + '.fxe')).unlink(missing_ok=True)
            print(f'{name}: blocked ({error})', flush=True)
            return dict(name=name, status='blocked', runtime='not tested',
                        sources=[], bytes=None, reason=str(error), log=str(log),
                        requirements=['hosted networking'] if name in NETWORK else [])

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        report = list(pool.map(compile_one, selected))
    if args.all:
        (build / 'application-report.json').write_text(json.dumps(report, indent=2) + '\n')
    elif (build / 'application-report.json').exists():
        # A named rebuild must update the install inventory and its coverage
        # fingerprints as well as the individual cache stamps.
        previous = json.loads((build / 'application-report.json').read_text())
        merged = {item['name']: item for item in previous}
        merged.update({item['name']: item for item in report})
        (build / 'application-report.json').write_text(
            json.dumps([merged[name] for name in sorted(merged)], indent=2) + '\n')
    built = sum(item['status'] == 'built' for item in report)
    print(f'Fuse-X applications: {built}/{len(report)} built')
    # An inventory must finish even with blockers; named builds are strict.
    return int(not args.all and built != len(report))


if __name__ == '__main__':
    sys.exit(main())
