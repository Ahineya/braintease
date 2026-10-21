"""Build the existing Bourne shell and utilities with static guest libc."""
import argparse
import hashlib
import importlib.util
import json
import fcntl
from pathlib import Path
import subprocess
import sys
from boot import ROOT, HERE, HEAP_PROFILE

# Keep the tiny RAM/bootstrap profile; the offline collection uses disk root.
NAMES = ('sh', 'echo', 'cat', 'ls', 'wc', 'sleep', 'true', 'false', 'cp', 'test')
NETWORK_NAMES = ('ifconfig', 'ping', 'telnet', 'telnetd', 'host', 'nslookup', 'dig', 'netcheck')


def build_apps(build, profile="minimal", networking=False, system_services=False):
    if profile not in ("minimal", "offline", "system"):
        raise RuntimeError("unknown x64 app profile")
    build.mkdir(parents=True, exist_ok=True)
    # Normal and acceptance images can share this cache safely.
    with (build / 'applications.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        return _build_apps(build, profile, networking, system_services)


def _build_apps(build, profile, networking=False, system_services=False):
    spec = importlib.util.spec_from_file_location('hosted_apps', ROOT / 'fuse-x/apps.py')
    recipes = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(recipes)
    directory = build / 'apps'
    directory.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    digest.update(f'heap-profile={HEAP_PROFILE}'.encode())
    digest.update(b'native-memory=checked')
    paths = [ROOT / 'fuse-x/cc.py', ROOT / 'fuse-x/apps.py', ROOT / 'fuse-x/libc/bridge.c',
             ROOT / 'fuse-x/heap_profile.py',
             ROOT / 'fuse-x/include/memory-layout.h', HERE / 'app_link.py', Path(__file__), *sorted((ROOT / 'ports/services').glob('*.h')),
             ROOT / 'ports/fuzix/ripple/userlib.c', ROOT / 'ports/fuzix/upstream/Kernel/include/userstructs.h']
    for root in ('runtime/include', 'ports/fuzix/ripple', 'ports/fuzix/upstream/Library', 'libraries/unicode'):
        paths += [p for p in (ROOT / root).rglob('*') if p.suffix in ('.c', '.h', '.inc')]
    for p in sorted(set(paths)):
        digest.update(str(p).encode()); digest.update(p.read_bytes())
    for name in ('rcc', 'rcc-native-object'):
        p = ROOT / 'target/release' / name
        digest.update(str((p.stat().st_size, p.stat().st_mtime_ns)).encode())
    sdk = digest.digest()
    excluded = {name: 'networking' for name in recipes.NETWORK | {'chat'}}
    if networking:
        for name in NETWORK_NAMES:
            excluded.pop(name, None)
    excluded.update({name: 'separate RFL packaging' for name in ('rift', 'vi')})
    excluded['mux'] = 'terminal multiplexer needs x64 terminal/PTY acceptance'
    selected = list(NAMES) if profile in ('minimal', 'system') else [n for n in recipes.names() if n not in excluded]
    if profile == 'system':
        selected = list(dict.fromkeys([*selected, 'tree', 'getopt', 'cron', 'crond', 'at', 'atrun', 'cal']))
    if networking:
        selected = list(dict.fromkeys([*selected, *NETWORK_NAMES]))
    if system_services:
        selected = list(dict.fromkeys([*selected, 'mkdir', 'chmod', 'rm', 'cmp', 'dirname', 'svcd', 'svc', 'console-session', 'net-ready', 'entropycheck', 'wifi', 'wifid', 'reboot']))
    # Alias packages copy their canonical output, and are installed as hard links.
    selected.sort(key=lambda n: (n in recipes.ALIASES, n))
    names = (*selected, 'appcheck', 'abicheck', 'timercheck', 'diskcheck')
    report = []
    logs = build / 'application-logs'; logs.mkdir(exist_ok=True)
    for name in names:
        if name in ('svcd', 'svc', 'console-session'):
            source = {'svcd':'manager.c', 'svc':'client.c', 'console-session':'console.c'}[name]
            sources = [ROOT / 'ports/services' / source, ROOT / 'ports/services/config.c', ROOT / 'libraries/ini/ini.c']
            flags = ['-I' + str(ROOT / 'libraries/ini')]
        elif name == 'entropycheck':
            sources, flags = [ROOT / 'ports/fuzix/ripple/entropycheck.c'], []
        elif name in ('wifi', 'wifid', 'reboot'):
            sources, flags = [HERE / 'userland' / (name+'.c')], []
        elif name == 'net-ready':
            sources, flags = [HERE / 'userland/net-ready.c'], []
        elif name == 'netcheck':
            sources, flags = [HERE / 'tests/netcheck.c'], ['-I' + str(recipes.PORT / 'ripple/include')]
        elif name == 'appcheck':
            sources, flags = [HERE / 'tests/appcheck.c'], []
        elif name == 'abicheck':
            sources, flags = [ROOT / 'c-test/tests/core/functions/test_native_application.c'], []
        elif name == 'timercheck':
            sources, flags = [HERE / 'tests/timercheck.c'], []
        elif name == 'diskcheck':
            sources, flags = [HERE / 'tests/diskcheck.c'], []
        else:
            sources, flags = recipes.recipe(name)
        key = hashlib.sha256(sdk + str(flags).encode())
        for p in sorted(set(sources + [h for s in sources for h in s.parent.glob('*.h')])):
            key.update(str(p).encode()); key.update(p.read_bytes())
        output = directory / f'{name}.fxe'
        stamp = directory / f'{name}.sha256'
        fingerprint = key.hexdigest()
        if output.exists() and stamp.exists() and stamp.read_text() == fingerprint:
            report.append(dict(name=name, status='built', cached=True, bytes=output.stat().st_size))
            continue
        canonical = recipes.ALIASES.get(name)
        if canonical in selected:
            target = directory / f'{canonical}.fxe'
            if not target.exists():
                raise RuntimeError(f'missing alias target {canonical}')
            output.write_bytes(target.read_bytes()); stamp.write_text(fingerprint)
            report.append(dict(name=name, status='built', alias_of=canonical, bytes=output.stat().st_size))
            continue
        if name == 'sh':
            # Same explicit stdin sentinel adaptation as the hosted recipe.
            original = recipes.PORT / 'upstream/Applications/V7/cmd/sh/main.c'
            text = original.read_text()
            if text.count('comdiv--;') != 1:
                raise RuntimeError('Bourne shell stdin sentinel adaptation needs review')
            generated = build / 'sh-main.c'
            generated.write_text(text.replace('comdiv--;', 'comdiv = (char *)-1;'))
            sources = [generated if s == original else s for s in sources]
        if name == 'awk':
            # Bootstrap frontend emits integer carrier 1 for a double return.
            # Expected-correct RED rct: test_native_recursive_double_power.
            original = recipes.PORT / 'awk/run.c'
            text = original.read_text()
            before = 'if (n <= 0)\n\t\treturn 1;'
            if text.count(before) != 1:
                raise RuntimeError('awk double-return bootstrap adaptation needs review')
            generated = build / 'awk-run.c'
            generated.write_text(text.replace(before, 'if (n <= 0)\n\t\treturn 1.0;'))
            sources = [generated if s == original else s for s in sources]
        command = [sys.executable, str(ROOT / 'fuse-x/cc.py'), *map(str, sources), *flags,
                   '--static', '--heap-profile', HEAP_PROFILE,
                   '--linker', str(HERE / 'app_link.py'), '--build', str(build), '-o', str(output)]
        print(f'Building x64 {name}', flush=True)
        with (logs / f'{name}.log').open('w') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            output.unlink(missing_ok=True); stamp.unlink(missing_ok=True)
            report.append(dict(name=name, status='blocked', log=str(logs / f'{name}.log')))
        else:
            stamp.write_text(fingerprint)
            report.append(dict(name=name, status='built', bytes=output.stat().st_size))
    inventory = dict(profile=profile, networking=networking, applications=report, excluded=excluded,
                     runtime='not tested by compilation')
    (build / 'application-report.json').write_text(json.dumps(inventory, indent=2) + '\n')
    blocked = [r['name'] for r in report if r['status'] != 'built']
    if blocked:
        (directory / 'manifest.json').unlink(missing_ok=True)
        raise RuntimeError(f"x64 applications failed: {', '.join(blocked)}; inspect {logs}")
    (directory / 'manifest.json').write_text(json.dumps(names) + '\n')
    return directory


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=('minimal', 'offline'), default='offline')
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    build_apps(args.build.resolve(), args.profile)
