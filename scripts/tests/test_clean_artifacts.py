#!/usr/bin/env python3
"""Exercise cleanup safety on a private Git repository, never real build outputs."""
import contextlib
import fcntl
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('cleanup', Path(__file__).resolve().parents[1] / 'clean-artifacts.py')
cleanup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cleanup)


class CleanupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='cleanup-safety-')
        self.root = Path(self.temp.name).resolve()
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        (self.root / '.gitignore').write_text('build\ntarget\n')
        self.previous = Path.cwd()
        self.age = time.time() - 7200

    def tearDown(self):
        os.chdir(self.previous)
        self.temp.cleanup()

    def file(self, name, content='data', old=True):
        p = self.root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
        if old:
            os.utime(p, (self.age, self.age))
        return p

    def run_cleanup(self, apply=False, active=()):
        argv = ['clean-artifacts.py', '--min-age-hours', '1'] + (['--apply'] if apply else [])
        with patch.object(cleanup, 'ROOT', self.root), patch.object(cleanup, 'active_paths', return_value=set(active)), patch.object(sys, 'argv', argv), contextlib.redirect_stdout(io.StringIO()):
            cleanup.main()

    def test_preview_and_apply_preserve_sources_disks_active_and_tracked_outputs(self):
        generated = self.file('build/probe/output.ir')
        source = self.file('build/probe/experiment.c')
        disk = self.file('build/probe/test.img')
        old_backup = self.file('build/probe/test.img.bak')
        latest_backup = self.file('build/probe/test.img.bak.1')
        os.utime(latest_backup, (self.age + 1, self.age + 1))
        canonical = self.file('build/fuzix/kernel.ir')
        active = self.file('build/live/output.ir')
        recent = self.file('build/recent/output.ir', old=False)
        tracked = self.file('build/tracked/source.asm')
        protected = self.file('build/tracked/output.ir')
        subprocess.run(['git', '-C', str(self.root), 'add', '-f', str(tracked)], check=True)
        link = self.root / 'build/probe/alias.ir'
        link.symlink_to(source)
        self.run_cleanup(active=[active])
        self.assertTrue(generated.exists())
        self.run_cleanup(apply=True, active=[active])
        self.assertFalse(generated.exists())
        self.assertFalse(old_backup.exists())
        for p in [source, disk, latest_backup, canonical, active, recent, tracked, protected, link]:
            self.assertTrue(p.exists(), str(p))

    def test_busy_cargo_profile_blocks_cache_deletion(self):
        tag = self.file('build/toolchain/CACHEDIR.TAG')
        compiled = self.file('build/toolchain/release/test-binary')
        lockpath = self.file('build/toolchain/release/.cargo-lock')
        with lockpath.open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.run_cleanup(apply=True)
            self.assertTrue(compiled.exists())
        self.run_cleanup(apply=True)
        self.assertFalse(compiled.exists())
        self.assertTrue(lockpath.exists(), 'Cargo lock inode must survive cleanup')
        self.assertFalse(tag.exists())

    def test_daily_cleanup_skips_inventory_after_a_successful_pass(self):
        self.file('build/probe/output.ir')
        self.run_cleanup(apply=True)
        later = self.file('build/probe/later.ir')
        with patch.object(cleanup, 'ROOT', self.root), patch.object(cleanup, 'active_paths', side_effect=AssertionError('must skip inventory')), patch.object(sys, 'argv', ['clean-artifacts.py', '--apply', '--daily', '--quiet']):
            cleanup.main()
        self.assertTrue(later.exists())
        self.run_cleanup(apply=True)
        self.assertFalse(later.exists())

    def test_process_arguments_protect_absolute_and_incremental_targets(self):
        output = f'123 /toolchain/bin/rustc -C incremental={self.root}/target/debug/incremental --out-dir {self.root}/target/debug/deps\n124 python3 build.py --output {self.root}/build/live/result.ir\n'
        responses = [subprocess.CompletedProcess([], 0, output, ''), subprocess.CompletedProcess([], 0, '', '')]
        with patch.object(cleanup, 'ROOT', self.root), patch.object(cleanup.subprocess, 'run', side_effect=responses):
            paths = cleanup.active_paths()
        self.assertTrue(cleanup.is_active(self.root / 'target/debug/incremental/crate', paths))
        self.assertTrue(cleanup.is_active(self.root / 'build/live', paths))


if __name__ == '__main__':
    unittest.main()
