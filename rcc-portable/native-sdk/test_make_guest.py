#!/usr/bin/env python3
"""Rebuild native tool sources with guest Make in disposable x86 Riplix.

The frontend, QBE and assembler remain bootstrap seeds. Each completed tool is
checkpointed after clean shutdown, filesystem checking and same-object host
link parity. Interrupted runs resume only when all input/output hashes match.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import fcntl
import hashlib
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent / 'qbe'))
from test_x86 import file, run_guest
from test_tools import discard_guest_disks, exercise
from extract_guest import extract_files

NAMES = ['rcc-x64-ar', 'rcc-native-cc', 'rcc-x64-ld', 'rcc-qbe']
SDK_OBJECTS = ['libc.a', 'crt0-0.o', 'crt0-2.o', 'crt0-3.o']


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fingerprint(files, images, rows):
    h = hashlib.sha256()
    for f in sorted(files, key=lambda f: f['name']):
        # Hash lengths and names separately; concatenating raw file contents
        # without boundaries could alias different sets of inputs.
        h.update(json.dumps([f['name'], f['mode'], digest(Path(f['path']))]).encode())
    for image in images:
        h.update(digest(image).encode())
    h.update(json.dumps(rows, sort_keys=True).encode())
    for harness in [Path(__file__), HERE / 'extract_guest.py', HERE / 'test_tools.py',
                    HERE.parent / 'qbe/test_x86.py']:
        h.update(harness.read_bytes())
    return h.hexdigest()


def cached_tool(directory, key, outputs):
    try:
        record = json.loads((directory / 'result.json').read_text())
        if (isinstance(record, dict) and isinstance(record.get('outputs'), dict)
                and record['key'] == key and record['status'] == 'passed'
                and record.get('host_link_parity') is True
                and set(record['outputs']) == set(outputs)
                and all(digest(directory / n) == v for n, v in record['outputs'].items())):
            return record
    except (OSError, ValueError, KeyError, TypeError):
        pass
    return None


def save_record(path, record):
    pending = path.with_suffix('.tmp')
    pending.write_text(json.dumps(record, indent=2) + '\n')
    pending.replace(path)


def rebuild(name, rows, files, key, args):
    directory = args.results / name
    directory.mkdir(parents=True, exist_ok=True)
    members = [Path(r['object']).name for r in rows if r['tool'] == name]
    executable = name + '.fxe'
    outputs = members + [executable]
    cached = cached_tool(directory, key, outputs)
    if cached:
        print(name + ': verified guest source build reused', flush=True)
        return cached
    target = 'build/bin/' + name
    make = 'make -r SDK=/tmp/sdk TOOLS=/bin CFLAGS=-v '
    commands = [
        ('cd /tmp/sdk; make -rn; echo SDK_RULES:$?', 'SDK_RULES:0'),
        ('cd /tmp/native-tools; make -rn SDK=/tmp/sdk TOOLS=/bin; echo TOOL_RULES:$?', 'TOOL_RULES:0'),
        (make + target + '; echo REBUILD:$?', 'REBUILD:0'),
        ('make -rq SDK=/tmp/sdk TOOLS=/bin CFLAGS=-v ' + target + '; echo UNCHANGED:$?', 'UNCHANGED:0'),
        getattr(args, 'tool_probe', (target + ' --help; echo HELP:$?', 'HELP:0'))]
    print(name + ': rebuilding raw C with guest Make', flush=True)
    disk, checks = run_guest(args.output, 'source-' + name,
                            files, commands, args.timeout)
    # Read the original build paths after clean shutdown. Copying every binary
    # and object to /tmp doubled their disk footprint and could fill the image
    # after a successful, expensive source build.
    extract_files(disk, directory, {executable: '/tmp/native-tools/' + target,
        **{n: '/tmp/native-tools/build/obj/' + n for n in members}})
    reference = directory / (name + '.host-link')
    subprocess.run([str(HERE.parent / 'build/rcc-x64-ld'), '--sdk', str(args.sdk_objects),
                    *[str(directory / n) for n in members], '-o', str(reference)], check=True)
    if reference.read_bytes() != (directory / executable).read_bytes():
        raise RuntimeError(name + ': guest/host link differs on the same objects')
    record = dict(status='passed', key=key, tool=name, commands=checks,
                  host_link_parity=True, outputs={n: digest(directory / n) for n in outputs})
    save_record(directory / 'result.json', record)
    if not args.keep_disks:
        discard_guest_disks(args.output, 'source-' + name)
    print(name + ': guest source build and same-object host link passed', flush=True)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', required=True, type=Path, help='prepared SDK headers/source/Makefile')
    parser.add_argument('--sdk-objects', type=Path, help='optional separately guest-rebuilt libc/startup directory')
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--results', type=Path, help='checkpoint/output directory (default: SOURCE/guest-build)')
    parser.add_argument('--seed-tools', type=Path, help='use a preceding generation of the four tools (NAME.fxe)')
    parser.add_argument('--qbe', type=Path, help='QBE bootstrap seed for source builds and execution checks')
    parser.add_argument('--compare-tools', type=Path, help='require byte-identical selected tools from an earlier generation')
    parser.add_argument('--tool', action='append', choices=NAMES, help='limit this run to selected tools; default all four')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/portable-native-x64')
    parser.add_argument('--timeout', type=int, default=7200, help='timeout per guest phase, in seconds')
    parser.add_argument('--jobs', type=int, choices=range(1, 4), default=1, help='parallel private tool-build guests')
    parser.add_argument('--keep-disks', action='store_true', help='retain successful disposable VM disks for debugging')
    args = parser.parse_args()
    args.sdk = args.sdk.resolve()
    args.sdk_objects = (args.sdk_objects or args.sdk).resolve()
    args.source = args.source.resolve()
    args.results = (args.results or args.source / 'guest-build').resolve()
    args.output = args.output.resolve()
    for directory in [args.seed_tools, args.compare_tools]:
        if directory and directory.resolve() == args.results:
            parser.error('seed/comparison generations must use a separate results directory')
    args.results.mkdir(parents=True, exist_ok=True)
    # Keep this descriptor live for the complete run. A second launcher must
    # fail before it can overwrite an active guest's disks or status records.
    build_lock = (args.output / '.native-tool-build.lock').open('a')
    try:
        fcntl.flock(build_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise RuntimeError('a native-tool build already owns this output directory') from None
    report = args.results / 'result.json'
    report.unlink(missing_ok=True)
    selected = [n for n in NAMES if not args.tool or n in args.tool]
    rows = json.loads((args.source / 'manifest.json').read_text())
    if set(r['tool'] for r in rows) != set(NAMES):
        raise ValueError('source manifest must cover exactly the four native tools')
    seed = ROOT / 'build/guest-native-assessment'
    qbe = (args.qbe or seed / 'qbe-x86.fxe').resolve()
    tools = {name: ((args.seed_tools / (name + '.fxe')) if args.seed_tools else
                    HERE.parent / 'build/apps' / (name + '-x86.fxe')) for name in NAMES}
    seed_inputs = {name: dict(path=str(path.resolve()), sha256=digest(path))
                   for name, path in tools.items()}
    files = [file('/bin/' + name, path) for name, path in tools.items()]
    files += [file('/bin/rcc-c', HERE.parent / 'build/apps/rcc-c-x86.fxe')]
    files.append(file('/bin/qbe', qbe))
    files += [file('/bin/' + name, seed / (name + '-x86.fxe'))
              for name in ['rcc-x64-as', 'make', 'mkdir']]
    for folder in ['include', 'tree', 'generated']:
        files += [file('/tmp/sdk/' + str(p.relative_to(args.sdk)), p)
                  for p in sorted((args.sdk / folder).rglob('*')) if p.is_file()]
    files.append(file('/tmp/sdk/Makefile', args.sdk / 'Makefile'))
    files += [file('/tmp/sdk/' + name, args.sdk_objects / name) for name in SDK_OBJECTS]
    files += [file('/tmp/native-tools/' + str(p.relative_to(args.source)), p)
              for p in sorted((args.source / 'src').rglob('*')) if p.is_file()]
    files.append(file('/tmp/native-tools/Makefile', args.source / 'Makefile'))
    images = [args.output / 'normal' / n for n in ['kernel.elf', 'boot.img', 'root.img']]
    key = fingerprint(files, images, rows)
    state = dict(status='running', key=key, jobs=args.jobs,
                 started_at=datetime.now(timezone.utc).isoformat(),
                 tools={n: dict(status='pending') for n in selected})
    state_path = args.results / 'run-state.json'
    save_record(state_path, state)
    # Start the largest groups first so the lowerer can make progress while
    # the smaller tools finish on the second worker. Each guest has its own
    # disk, firmware variables and log paths; only the base image is shared.
    order = sorted(selected, key=lambda n: sum((args.source / r['source']).stat().st_size
                   for r in rows if r['tool'] == n), reverse=True)
    records, failures = {}, {}
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        pending = {pool.submit(rebuild, n, rows, files, key, args): n for n in order}
        for future in as_completed(pending):
            name = pending[future]
            try:
                records[name] = future.result()
                state['tools'][name] = dict(status='passed', result=str(args.results / name / 'result.json'))
            except Exception as error:
                failures[name] = str(error)
                state['tools'][name] = dict(status='failed', error=str(error))
                print(name + ': FAILED: ' + str(error), flush=True)
            save_record(state_path, state)
    if failures:
        state['status'] = 'failed'
        save_record(state_path, state)
        raise RuntimeError('guest source builds failed: ' + ', '.join(failures))
    builds = [records[n] for n in selected]
    try:
        generation_reference = {}
        for name in selected:
            path = args.results / name / (name + '.fxe')
            tools[name] = path
            if args.compare_tools:
                reference = (args.compare_tools / (name + '.fxe')).resolve()
                if path.read_bytes() != reference.read_bytes():
                    raise RuntimeError(name + ': compiler generations differ')
                generation_reference[name] = dict(path=str(reference), sha256=digest(reference),
                                                  byte_identical=True)
            # Stable flat output names can seed the next generation.
            (args.results / (name + '.fxe')).write_bytes(path.read_bytes())
        state['phase'] = 'execution'
        save_record(state_path, state)
        # Exercise the rebuilt set, then its program after a fresh boot.
        execution = exercise(args.sdk, tools, args.output, args.results, args.timeout,
                             qbe, args.sdk_objects, args.keep_disks)
        save_record(report, dict(status='passed', tools=selected, full_source_rebuild=len(selected) == len(NAMES),
            key=key, builds=builds, execution=execution, generation_comparison=bool(args.compare_tools),
            seed_inputs=seed_inputs, generation_reference=generation_reference,
            execution_tools={n: dict(path=str(p), sha256=digest(p), guest_rebuilt=n in selected)
                             for n, p in tools.items()},
            sdk_inputs={n: dict(path=str(args.sdk_objects / n), sha256=digest(args.sdk_objects / n)) for n in SDK_OBJECTS},
            qbe_input=dict(path=str(qbe), sha256=digest(qbe)),
            provenance='selected tools rebuilt from raw C by guest Make; frontend/QBE/assembler and kernel remain bootstrap seeds'))
    except Exception as error:
        state.update(status='failed', error=str(error))
        save_record(state_path, state)
        raise
    state['status'] = 'passed'
    save_record(state_path, state)
    print('Guest Make source build, same-object link parity, compile/archive/link/run and fresh boot passed: '
          + ', '.join(selected), flush=True)


if __name__ == '__main__':
    main()
