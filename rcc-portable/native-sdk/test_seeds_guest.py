#!/usr/bin/env python3
"""Rebuild one remaining bootstrap tool from raw C in a private x86 guest.

Reuse the accepted four-tool generation where possible. Explicit overrides
identify newly bootstrapped language fixes; they never rewrite accepted seeds.
"""
import argparse
from datetime import datetime, timezone
import fcntl
import hashlib
import json
from pathlib import Path
import sys

from source_seeds import TOOLS
from test_make_guest import rebuild, digest, fingerprint, save_record
from test_tools import exercise, discard_guest_disks
from test_x86 import file, run_guest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
NATIVE = ['rcc-x64-ar', 'rcc-native-cc', 'rcc-x64-ld', 'rcc-qbe']


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tool', choices=TOOLS, required=True)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--sdk', type=Path, required=True)
    p.add_argument('--sdk-objects', type=Path)
    p.add_argument('--seed-tools', type=Path, required=True)
    p.add_argument('--lowerer', type=Path)
    p.add_argument('--linker', type=Path)
    p.add_argument('--qbe', type=Path, required=True)
    p.add_argument('--frontend', type=Path, default=HERE.parent / 'build/apps/rcc-c-x86.fxe')
    p.add_argument('--assembler', type=Path, default=ROOT / 'build/guest-native-assessment/rcc-x64-as-x86.fxe')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--results', type=Path, required=True)
    p.add_argument('--timeout', type=int, default=28800)
    p.add_argument('--keep-disks', action='store_true')
    p.add_argument('--reuse-record', type=Path,
                   help='explicit verified stopped-build input/object record (incremental build)')
    a = p.parse_args()
    for n in ['source', 'sdk', 'seed_tools', 'qbe', 'frontend', 'assembler', 'output', 'results']:
        setattr(a, n, getattr(a, n).resolve())
    a.sdk_objects = (a.sdk_objects or a.sdk).resolve()
    if a.results == a.seed_tools or a.results == a.source:
        p.error('results must be separate from seeds and source')
    a.output.mkdir(parents=True, exist_ok=True)
    a.results.mkdir(parents=True, exist_ok=True)
    lock = (a.output / '.native-tool-build.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    rows = json.loads((a.source / 'manifest.json').read_text())
    if not rows or {r['tool'] for r in rows} != {a.tool}:
        raise ValueError('source manifest must contain exactly the selected tool')
    tools = {n: a.seed_tools / (n + '.fxe') for n in NATIVE}
    if a.lowerer:
        tools['rcc-qbe'] = a.lowerer.resolve()
    if a.linker:
        tools['rcc-x64-ld'] = a.linker.resolve()
    seeds = {**tools, 'rcc-c': a.frontend, 'rcc-x64-as': a.assembler, 'qbe': a.qbe}
    files = [file('/bin/' + n, path) for n, path in seeds.items()]
    for n in ['make', 'mkdir']:
        files.append(file('/bin/' + n, ROOT / 'build/guest-native-assessment' / (n + '-x86.fxe')))
    for folder in ['include', 'tree', 'generated']:
        files += [file('/tmp/sdk/' + str(f.relative_to(a.sdk)), f)
                  for f in sorted((a.sdk / folder).rglob('*')) if f.is_file()]
    files += [file('/tmp/sdk/Makefile', a.sdk / 'Makefile')]
    files += [file('/tmp/sdk/' + n, a.sdk_objects / n) for n in ['libc.a', 'crt0-0.o', 'crt0-2.o', 'crt0-3.o']]
    files += [file('/tmp/native-tools/' + str(f.relative_to(a.source)), f)
              for f in sorted(a.source.rglob('*')) if f.is_file()]
    probes = ['aggregate', 'conversions', 'bitfields', 'pointer_difference']
    # Long source builds must execute the exact regression inputs fingerprinted
    # at launch, even if someone edits the repository before the final phase.
    fixtures = a.results / 'fixtures'
    fixtures.mkdir(exist_ok=True)
    for n in probes:
        source = fixtures / (n + '.c')
        source.write_bytes((HERE.parent / ('native/tests/' + n + '.c')).read_bytes())
        files.append(file('/' + n + '.c', source))
    reuse = None
    if a.reuse_record:
        from reuse_objects import prepare_reuse
        files, reuse = prepare_reuse(a.reuse_record.resolve(), a.source, rows, files, a.results)
    base = fingerprint(files, [a.output / 'normal' / n for n in ['kernel.elf', 'boot.img', 'root.img']], rows)
    key = hashlib.sha256(base.encode() + Path(__file__).read_bytes()).hexdigest()
    state_path = a.results / 'run-state.json'
    state = dict(status='running', tool=a.tool, key=key, source_units=len(rows), phase='aggregate-preflight',
                 started_at=datetime.now(timezone.utc).isoformat())
    save_record(state_path, state)
    (a.results / 'result.json').unlink(missing_ok=True)
    try:
        phase = 'aggregate-preflight-' + a.tool
        _, preflight = run_guest(a.output, phase, files, [
            ('rcc-native-cc -v --tools /bin --sdk /tmp/sdk /aggregate.c -o /tmp/aggregate; echo COMPILE:$?', 'COMPILE:0'),
            ('/tmp/aggregate; echo AGGREGATE:$?', 'AGGREGATE:0')], a.timeout)
        if not a.keep_disks:
            discard_guest_disks(a.output, phase)
        state['phase'] = 'source-build'
        save_record(state_path, state)
        target = 'build/bin/' + a.tool
        if a.tool == 'rcc-c':
            a.tool_probe = (target + ' --target native smoke.c -o /tmp/probe.ir; echo PROBE:$?', 'PROBE:0')
        elif a.tool == 'qbe':
            a.tool_probe = (target + ' -h; echo HELP:$?', 'HELP:0')
        build = rebuild(a.tool, rows, files, key, a)
        if reuse:
            for name, checksum in reuse['objects'].items():
                if digest(a.results / a.tool / name) != checksum:
                    raise RuntimeError('recovered object changed during build: ' + name)
        candidate = a.results / a.tool / (a.tool + '.fxe')
        state['phase'] = 'execution'
        save_record(state_path, state)
        # Exercise the rebuilt stage itself on semantics required by these
        # sources, including cases a hello-world pipeline would never reach.
        replacements = {'/bin/' + a.tool: file('/bin/' + a.tool, candidate)}
        checks_files = [replacements.get(f['name'], f) for f in files]
        semantic_commands = []
        for n in probes:
            semantic_commands += [
                ('rcc-native-cc -v --tools /bin --sdk /tmp/sdk /' + n + '.c -o /tmp/' + n + '; echo COMPILE:$?', 'COMPILE:0'),
                ('/tmp/' + n + '; echo SEMANTIC:$?', 'SEMANTIC:0')]
        phase = 'semantic-' + a.tool
        _, semantics = run_guest(a.output, phase, checks_files, semantic_commands, a.timeout)
        if not a.keep_disks:
            discard_guest_disks(a.output, phase)
        execution = exercise(a.sdk, tools, a.output, a.results, a.timeout,
            candidate if a.tool == 'qbe' else a.qbe, a.sdk_objects, a.keep_disks,
            frontend=candidate if a.tool == 'rcc-c' else a.frontend,
            assembler=candidate if a.tool == 'rcc-x64-as' else a.assembler)
        flat = a.results / (a.tool + '.fxe')
        flat.write_bytes(candidate.read_bytes())
        save_record(a.results / 'result.json', dict(status='passed', key=key, tool=a.tool,
            source_units=len(rows), build=build, aggregate_preflight=preflight,
            semantics=semantics, execution=execution,
            seed_inputs={n: dict(path=str(f), sha256=digest(f)) for n, f in seeds.items()},
            sdk_inputs={n:digest(a.sdk_objects / n) for n in ['libc.a', 'crt0-0.o', 'crt0-2.o', 'crt0-3.o']},
            executable_sha256=digest(flat), generation_comparison=False,
            reused_source_objects=reuse,
            provenance=('incremental guest Make source build with explicitly verified recovered objects'
                        if reuse else 'selected tool rebuilt from raw C with guest Make')
                       + ', same-object host link and fresh-boot execution; other stage tools remain explicit seeds'))
        state['status'] = 'passed'
        print(a.tool + ': guest source rebuild, host link parity and combined fresh-boot execution passed', flush=True)
    except Exception as error:
        state.update(status='failed', error=str(error))
        raise
    finally:
        save_record(state_path, state)


if __name__ == '__main__':
    main()
