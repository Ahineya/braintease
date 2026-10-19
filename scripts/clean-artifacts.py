#!/usr/bin/env python3
"""Prune old image backups and idle experiment outputs; preview unless --apply.

Current OS builds, shared files, source files, guest disks and the main Cargo
artifact cache are preserved. Old Rust incremental sessions and separate Cargo
experiment targets can be rebuilt and are eligible when idle. Active compiler
and guest command lines/open files protect their build directories.
"""
import argparse
import collections
import fcntl
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
EXTENSIONS = set('.asm .ir .pobj .par .bin .bf .bfm .bfa .map .ll .bc .o .obj .a .rlib .rmeta .dylib .so .d .native .out .rxe .fxe .wasm .exe .gcda .gcno'.split())
IMAGE = re.compile(r'\.(?:img|dsk|iso|qcow2?|vhdx?)(?:\.|$)', re.I)
BACKUP = re.compile(r'^(.*\.(?:img|dsk|iso|qcow2?|vhdx?)(?:\.[^.]+)?)\.bak(?:\..+)?$', re.I)
TOOLS = {'cargo', 'rustc', 'rcc', 'rcpp', 'rfl', 'rasm', 'rlink', 'make', 'gmake', 'rvm', 'rbf', 'fuse-x', 'python', 'python3', 'Python', 'node'}


def under(path, parent):
    return path == parent or parent in path.parents


def active_paths():
    process = subprocess.run(['ps', '-axo', 'pid=,args='], capture_output=True, text=True, timeout=10)
    if process.returncode:
        raise RuntimeError('Cannot inspect active builds; no cleanup performed')
    paths, pids = set(), []
    for line in process.stdout.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) < 2 or int(parts[0]) == os.getpid():
            continue
        command = Path(parts[1].split(None, 1)[0]).name
        executable = Path(parts[1].split(None, 1)[0])
        if command not in TOOLS and not command.startswith('python3.') and not under(executable, ROOT):
            continue
        pids.append(parts[0])
        for word in re.findall(r'(?:/[^\s\'\"]+|(?:build|target|fuse-x/build|rcc-portable/build|ports/doom/build)/[^\s\'\"]+)', parts[1]):
            p = Path(word.rstrip(',;'))
            p = p if p.is_absolute() else ROOT / p
            if under(p, ROOT):
                paths.add(p)
    if pids:
        # Also finds Cargo targets selected through CARGO_TARGET_DIR rather than argv.
        opened = subprocess.run(['lsof', '-a', '-p', ','.join(pids), '-Fn'], capture_output=True, text=True, timeout=15)
        if opened.returncode not in (0, 1):
            raise RuntimeError('Cannot inspect active build files; no cleanup performed')
        for line in opened.stdout.splitlines():
            if line.startswith('n/'):
                p = Path(line[1:])
                if under(p, ROOT):
                    paths.add(p)
    return paths


def is_active(directory, references):
    return any(under(p, directory)
               or (p.name in {'.cargo-lock', 'incremental'} and under(directory, p.parent if p.name == '.cargo-lock' else p))
               for p in references)


def lock_cache(directory):
    """Hold Cargo's profile lock while deleting cache entries, including on races."""
    if directory.parent.name == 'incremental':
        profiles = [directory.parent.parent]
    else:
        profiles = [p for p in (directory / 'debug', directory / 'release') if p.is_dir()]
    held = []
    try:
        for profile in profiles:
            lock = (profile / '.cargo-lock').open('a')
            held.append(lock)
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return held
    except BlockingIOError:
        for lock in held:
            lock.close()
        return None


def files_in(directory):
    for base, dirs, files in os.walk(directory):
        dirs[:] = [d for d in dirs if not (Path(base) / d).is_symlink() and d != '.git']
        for name in files:
            p = Path(base) / name
            try:
                info = p.lstat()
            except FileNotFoundError:
                continue
            if stat.S_ISREG(info.st_mode):
                yield p, info


def candidates(root):
    protected = {'fuzix', 'rust-rcc', 'ros', 'doom', 'netsurf', 'tls', 'telegram-private'}
    for directory in (root / 'build', root / 'rcc-portable/build', root / 'ports/doom/build', root / 'c-test/build'):
        if not directory.is_dir():
            continue
        for p in directory.iterdir():
            if not p.is_dir() or p.is_symlink():
                continue
            if directory == root / 'build' and p.name in protected:
                continue
            if directory == root / 'rcc-portable/build' and p.name in {'apps', 'sdk', 'package-cache', 'host-generation', 'host-generation-final'}:
                continue
            if directory == root / 'ports/doom/build' and p.name in {'ripple', 'native', 'run', 'run-bf', 'assets', 'generated', 'rcc-native'}:
                continue
            yield p
    # Isolated temporary toolchains from prior native-host experiments.
    for name in ('hostfs-runtime',):
        p = root / 'fuse-x/build' / name
        if p.is_dir():
            yield p
    # Keep compiled dependencies and binaries. Only idle incremental sessions expire.
    for directory in (root / 'target/debug/incremental', root / 'target/release/incremental'):
        if directory.is_dir():
            yield from (p for p in directory.iterdir() if p.is_dir() and not p.is_symlink())


def clean(args):
    os.chdir(ROOT)
    cutoff = time.time() - args.min_age_hours * 3600
    tracked = set(subprocess.check_output(['git', 'ls-files', '-z']).decode().split('\0'))
    tracked_parents = {parent for name in tracked if name for parent in (ROOT / name).parents}
    active = active_paths()
    planned, summaries, skipped, held_locks = {}, [], 0, []

    for directory in candidates(ROOT):
        if is_active(directory, active):
            skipped += 1
            continue
        relative = str(directory.relative_to(ROOT))
        ignored = subprocess.run(['git', 'check-ignore', '-q', '--', relative]).returncode == 0
        if not ignored or directory in tracked_parents:
            continue
        if not args.quiet:
            print('Checking ' + relative, flush=True)
        entries = list(files_in(directory))
        if not entries or max(s.st_mtime for _, s in entries) > cutoff:
            continue
        cargo = (directory / 'CACHEDIR.TAG').exists() or (directory / '.rustc_info.json').exists()
        incremental = directory.parent.name == 'incremental'
        if args.apply and (cargo or incremental):
            locks = lock_cache(directory)
            if locks is None:
                skipped += 1
                continue
            held_locks.extend(locks)
        chosen = [(p, s) for p, s in entries if not IMAGE.search(p.name)
                  and not p.name.endswith('.lock') and p.name != '.cargo-lock'
                  and (cargo or incremental or p.suffix in EXTENSIONS)]
        for p, info in chosen:
            planned[p] = (info, directory)
        if chosen:
            summaries.append((relative, len(chosen), sum(s.st_blocks * 512 for _, s in chosen)))

    # Include image backup families in ignored build trees, never source-file .bak.
    families = collections.defaultdict(list)
    for directory in (ROOT / 'build', ROOT / 'fuse-x/build', ROOT / 'ports/fuzix/build', ROOT / 'rcc-portable/build'):
        if directory.is_dir():
            for p, info in files_in(directory):
                match = BACKUP.match(p.name)
                if match:
                    families[p.parent / match[1]].append((p, info))
    for disk, entries in families.items():
        entries.sort(key=lambda v: v[1].st_mtime_ns, reverse=True)
        removed = entries[args.keep_backups:]
        if is_active(disk.parent, active):
            # Normal OS builders may be writing a backup in this directory now.
            skipped += 1
            continue
        for p, info in removed:
            if str(p.relative_to(ROOT)) not in tracked and p not in active:
                planned[p] = (info, disk.parent)
        if removed:
            summaries.append((str(disk.relative_to(ROOT)) + ' backups', len(removed), sum(s.st_blocks * 512 for _, s in removed)))

    allocated = sum(info.st_blocks * 512 for info, _ in planned.values())
    if not args.quiet:
        for name, count, size in summaries:
            print(f'{size / 1024**3:7.2f} GiB  {count:6} files  {name}', flush=True)
    if args.apply:
        # Resolve once more immediately before unlinking; never follow a changed link.
        removed = removed_bytes = 0
        active = active_paths()
        active_owners = {owner for owner in {owner for _, owner in planned.values()} if is_active(owner, active)}
        for p, (before, owner) in planned.items():
            if owner in active_owners:
                continue
            try:
                now = p.lstat()
            except FileNotFoundError:
                continue
            if (not p.is_symlink() and (now.st_dev, now.st_ino, now.st_mtime_ns, now.st_size)
                    == (before.st_dev, before.st_ino, before.st_mtime_ns, before.st_size)):
                p.unlink()
                removed += 1
                removed_bytes += now.st_blocks * 512
        for lock in held_locks:
            lock.close()
        if removed or not args.quiet:
            print(f'Cleanup: removed {removed} generated files ({removed_bytes / 1024**3:.2f} GiB allocated); '
                  f'{shutil.disk_usage(ROOT).free / 1024**3:.2f} GiB free; {skipped} active directories preserved', flush=True)
    elif not args.quiet:
        print(f'Preview: {len(planned)} files, {allocated / 1024**3:.2f} GiB allocated. '
              f'{skipped} active directories preserved. Add --apply to clean.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', help='delete the previewed categories')
    parser.add_argument('--min-age-hours', type=float, default=24, help='minimum idle age for experiment outputs (default 24)')
    parser.add_argument('--keep-backups', type=int, default=1, help='recovery copies per image (default 1)')
    parser.add_argument('--daily', action='store_true', help='with --apply, run at most once per day')
    parser.add_argument('--quiet', action='store_true')
    args = parser.parse_args()
    if not math.isfinite(args.min_age_hours) or args.min_age_hours < 0 or args.keep_backups < 0:
        parser.error('age and backup count must not be negative')
    if not args.apply:
        clean(args)
        return
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    with (build / '.artifact-cleanup.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            if not args.quiet:
                print('Another artifact cleanup is running; skipped')
            return
        stamp = build / '.artifact-cleanup.json'
        if args.daily and stamp.exists() and time.time() - stamp.stat().st_mtime < 86400:
            return
        clean(args)
        temporary = stamp.with_suffix('.tmp')
        temporary.write_text(json.dumps(dict(completed_at=time.time(), min_age_hours=args.min_age_hours)) + '\n')
        temporary.replace(stamp)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        sys.exit(f'Artifact cleanup: {error}')
