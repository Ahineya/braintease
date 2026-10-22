#!/usr/bin/env python3
"""Check pinned extraction backends against EJS vectors on host and guest QuickJS."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
PIN = json.loads((HERE / 'UPSTREAM.json').read_text())


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def result(text):
    records = re.findall(r'^EXTRACT_RESULT (.+)$', text, re.M)
    if len(records) != 1:
        raise AssertionError('Missing or duplicate extraction result\n' + text[-4000:])
    record = json.loads(records[0])
    if record['checks'] < 1 or record['checks'] != len(record['results']):
        raise AssertionError('No extraction checks')
    return record


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, default=ROOT / 'build/ytmusic-extraction')
    ap.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    ap.add_argument('--skip-build', action='store_true')
    ap.add_argument('--host-only', action='store_true')
    ap.add_argument('--memory-mode', choices=('checked', 'read-v1'), default='checked')
    ap.add_argument('--cached', action='store_true', help='diagnostic only: preprocess on host, test prepared-player execution in guest')
    ap.add_argument('--backend', choices=('ejs', 'youtubejs'), default='ejs')
    ap.add_argument('--label', default='', help='retain separately named benchmark reports/bundles (letters, digits and hyphens)')
    args = ap.parse_args()
    if args.label and not re.fullmatch(r'[A-Za-z0-9-]+', args.label):
        ap.error('--label must contain only letters, digits and hyphens')
    os.environ['RCC_NATIVE_MEMORY'] = args.memory_mode
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    # Labels share probe SDKs. Refuse concurrent rebuilds/runs rather than
    # measuring a moving executable or attributing another run's hash to it.
    lock = (build / '.extraction-check.lock').open('a')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise SystemExit('Another extraction check is using this build directory; wait for it to finish')
    upstream, dependencies = build / 'ejs', build / 'bundler'
    suffix = ('-read-v1' if args.memory_mode == 'read-v1' else '') + ('-cached' if args.cached else '')
    binary_suffix = suffix
    if args.backend == 'youtubejs':
        suffix += '-youtubejs'
    if args.label:
        suffix += '-' + args.label
    report = build / ('report' + suffix + '.json')
    report.unlink(missing_ok=True)
    commit = subprocess.check_output(['git', '-C', str(upstream), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != PIN['ejs']['commit']:
        raise SystemExit('EJS commit differs from pin')
    if subprocess.check_output(['git', '-C', str(upstream), 'status', '--porcelain', '--untracked-files=no']):
        raise SystemExit('EJS tracked sources were modified')
    if args.backend == 'youtubejs':
        tree = build / 'youtubejs'
        if subprocess.check_output(['git', '-C', str(tree), 'rev-parse', 'HEAD'], text=True).strip() != PIN['youtubejs']['commit']:
            raise SystemExit('YouTube.js commit differs from pin')
        if subprocess.check_output(['git', '-C', str(tree), 'status', '--porcelain', '--untracked-files=no']):
            raise SystemExit('YouTube.js tracked sources were modified')
    players = []
    for pin in PIN['players']:
        if args.backend not in pin['backends']:
            continue
        file = build / pin.get('filename', 'player-' + pin['id'] + '.js')
        if sha(file) != pin['sha256']:
            raise SystemExit('Player fixture hash differs: ' + pin['id'])
        players.append((pin, file))
    sdk = build / ('quickjs' + binary_suffix)
    solver, host, guest_binary = build / ('solver' + suffix + '.js'), build / ('extract-host' + suffix), sdk / 'extract-probe.fxe'
    if not args.skip_build:
        subprocess.run(['node', str(HERE / 'bundle.mjs'), str(upstream), str(dependencies), str(solver),
                        *([str(build / 'youtubejs')] if args.backend == 'youtubejs' else [])], check=True)
        core = ROOT / 'ports/quickjs/upstream'
        command = ['cc', '-std=gnu11', '-O2', '-fwrapv', '-D_GNU_SOURCE', '-DCONFIG_VERSION="2026-06-04"',
                   '-I' + str(core), str(HERE / 'probe.c'),
                   *[str(core / (n + '.c')) for n in ('quickjs', 'dtoa', 'libregexp', 'libunicode', 'cutils')], '-lm', '-o', str(host)]
        with (build / 'host-build.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=log, check=True, timeout=180)
        if not args.host_only:
            sys.path.insert(0, str(ROOT / 'ports/quickjs'))
            from explore import guest_build
            sdk.mkdir(parents=True, exist_ok=True)
            guest_build(sdk, args.compiler.resolve(), 'native', 'runtime',
                        application_sources=[HERE / 'probe.c'], output_name='extract-probe')
    extra = ['--prepared'] if args.cached else []
    if args.cached:
        prepare = """import fs from 'node:fs';import vm from 'node:vm';
const [bundle,input,output]=process.argv.slice(1),c={};vm.createContext(c);
vm.runInContext(fs.readFileSync(bundle,'utf8'),c,{timeout:30000});
c.player=fs.readFileSync(input,'utf8');
fs.writeFileSync(output,vm.runInContext('extractionProbe.prepare(player)',c,{timeout:30000}));"""
        prepared = []
        for pin, player in players:
            destination = build / ('prepared-' + pin['id'] + ('-youtubejs' if args.backend == 'youtubejs' else '') + '.js')
            subprocess.run(['node', '--input-type=module', '-e', prepare, str(solver), str(player), str(destination)], check=True)
            prepared.append((pin, destination))
        players = prepared
    identities = dict(compiler_sha256=sha(args.compiler.resolve()),
        probe_sha256=sha(guest_binary) if not args.host_only else None,
        engine_sha256=sha(sdk / 'source/quickjs.c') if not args.host_only else None,
        libc_sha256=sha(sdk / 'sdk-native/lib/libc.fxl.1') if not args.host_only else None,
        kernel_sha256=sha(Path(os.environ.get('FUSE_X_TEST_KERNEL', ROOT / 'fuse-x/build/fuse-x'))) if not args.host_only else None)
    records = []
    for pin, player in players:
        started = time.monotonic()
        output = subprocess.run([str(host), str(solver), str(player), pin['id'], *extra], capture_output=True, text=True, timeout=630)
        (build / ('host-' + pin['id'] + suffix + '.log')).write_text(output.stdout + output.stderr)
        if output.returncode:
            raise AssertionError('Host solver failed\n' + output.stderr[-3000:])
        reference = result(output.stdout)
        positive_seconds = time.monotonic() - started
        started = time.monotonic()
        negative = subprocess.run([str(host), str(solver), str(player), pin['id'], *extra, '--negative'], capture_output=True, text=True, timeout=630)
        (build / ('host-negative-' + pin['id'] + suffix + '.log')).write_text(negative.stdout + negative.stderr)
        if negative.returncode != 1 or 'EJS vector mismatch' not in negative.stderr or 'EXTRACT_RESULT' in negative.stdout:
            raise AssertionError('Host negative control did not fail correctly')
        records.append(dict(player=pin['id'], variant=pin['variant'],
                            fixture_sha256=pin['sha256'], input_sha256=sha(player), host=reference,
                            host_seconds=positive_seconds, host_negative_seconds=time.monotonic() - started,
                            host_negative='passed'))
        print(f"host: {pin['id']} {reference['checks']} exact upstream vectors; negative control passed", flush=True)
    if not args.host_only:
        sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
        from guest import Guest
        files = [dict(name='/tmp/solver.js', path=str(solver)),
                 dict(name='/lib/fuse-x/libc.fxl.1', path=str(sdk / 'sdk-native/lib/libc.fxl.1'))]
        files.extend(dict(name='/tmp/' + p.name, path=str(p)) for _, p in players)
        with tempfile.TemporaryDirectory(prefix='ytmusic-ejs-') as temporary:
            guest = Guest(temporary, 'native', {'extract-probe': guest_binary}, files=files, blocks=65536, timeout=1800)
            failure = None
            try:
                for record, (pin, player) in zip(records, players):
                    command = 'extract-probe /tmp/solver.js /tmp/' + player.name + ' ' + pin['id'] + (' --prepared' if args.cached else '')
                    print(f"native: {pin['id']} starting {'prepared execution' if args.cached else 'cold preprocessing'}; JS deadline 600 s", flush=True)
                    started = time.monotonic()
                    output = guest.shell(command + '; echo EXTRACT_STATUS:$?', seconds=660)
                    record['guest_seconds'] = time.monotonic() - started
                    resources = re.search(r'EXTRACT_RESOURCES peak_cells=(\d+) polls=(\d+) live=(\d+)', output)
                    if resources:
                        record['guest_resources'] = dict(peak_cells=int(resources[1]), polls=int(resources[2]), live=int(resources[3]))
                    if '\nEXTRACT_STATUS:0\n' not in output:
                        raise AssertionError('Guest solver failed\n' + output[-5000:])
                    actual = result(output)
                    for key in ('player', 'checks', 'playerLength', 'preparedLength', 'results'):
                        if actual[key] != record['host'][key]:
                            raise AssertionError('Guest differs from pristine host: ' + key)
                    record['guest'] = actual
                    if not resources or int(resources[3]):
                        raise AssertionError('Guest allocator teardown was not verified')
                    print(f"native: {pin['id']} positive vectors passed in {record['guest_seconds']:.2f} s; "
                          f"phases={actual.get('phases', {})}, peak_cells={resources[1]}; checking negative control", flush=True)
                    output = guest.shell(command + ' --negative; echo EXTRACT_STATUS:$?', seconds=660)
                    if '\nEXTRACT_STATUS:1\n' not in output or 'EJS vector mismatch' not in output or 'EXTRACT_RESULT' in output:
                        raise AssertionError('Guest negative control did not fail correctly\n' + output[-4000:])
                    record['guest_negative'] = 'passed'
                    print(f"native: {pin['id']} exact host/upstream outputs and failing negative control", flush=True)
            except Exception as error:
                failure = error
                # Restore the shell before shutdown if native parsing exceeded
                # the outer deadline without reaching a JS interrupt poll.
                os.write(guest.master, b'\x03')
                try:
                    guest.read(guest.prompt, 15)
                except (AssertionError, OSError):
                    pass
            finally:
                try:
                    guest.close()
                finally:
                    (build / ('native-guest' + suffix + '.log')).write_bytes(guest.log)
            if failure:
                report.write_text(json.dumps(dict(status='blocked',
                    scope='Cached execution diagnostic' if args.cached else 'Full guest preprocessing/execution',
                    ejs=commit, backend=args.backend,
                    youtubejs=PIN.get('youtubejs') if args.backend == 'youtubejs' else None,
                    native_memory_mode=args.memory_mode, error=str(failure), cases=records,
                    solver_sha256=sha(solver), guest_filesystem='passed', **identities), indent=2) + '\n')
                raise failure
    scope = 'Cached prepared-player execution only; preprocessing runs on host, not a self-contained extraction proof' if args.cached else 'Offline upstream player preprocessing and n/signature vectors; not live URL extraction or playback'
    report.write_text(json.dumps(dict(status='passed', scope=scope,
        ejs=commit, backend=args.backend, execution=['host'] if args.host_only else ['host', 'fuse-x'],
        youtubejs=PIN.get('youtubejs') if args.backend == 'youtubejs' else None,
        native_memory_mode=args.memory_mode, solver_sha256=sha(solver),
        quickjs=json.loads((ROOT / 'ports/quickjs/UPSTREAM.json').read_text()), cases=records,
        guest_filesystem='passed' if not args.host_only else 'not run', **identities), indent=2) + '\n')
    print(report)


if __name__ == '__main__':
    main()
