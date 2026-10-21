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

    def run_cleanup(self, apply=False, active=(), extra=()):
        argv = ['clean-artifacts.py', '--min-age-hours', '1'] + (['--apply'] if apply else []) + list(extra)
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

    def test_cache_only_prunes_idle_caches_and_preserves_backups_and_other_builds(self):
        macro = self.file('target/rfl-macros/old/macro')
        wrapper = self.file('target/rfl-macros/old/entry.rfl')
        incremental = self.file('target/debug/incremental/old/query-cache.bin')
        backup = self.file('build/probe/test.img.bak')
        newer_backup = self.file('build/probe/test.img.bak.1')
        other_build = self.file('build/probe/output.ir')
        release = self.file('target/release/rcc')
        recent = self.file('target/rfl-macros/recent/macro', old=False)
        active = self.file('target/rfl-macros/active/macro')
        self.run_cleanup(apply=True, active=[active], extra=['--caches-only'])
        self.assertFalse(macro.exists())
        self.assertFalse(incremental.exists())
        for p in (wrapper, backup, newer_backup, other_build, release, recent, active):
            self.assertTrue(p.exists(), str(p))

    def test_busy_macro_artifact_is_preserved_and_lock_inode_survives(self):
        macro = self.file('target/rfl-macros/old/macro')
        lockpath = self.file('target/rfl-macros/old/lock')
        inode = lockpath.stat().st_ino
        with lockpath.open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.run_cleanup(apply=True, extra=['--caches-only'])
            self.assertTrue(macro.exists())
        self.run_cleanup(apply=True, extra=['--caches-only'])
        self.assertFalse(macro.exists())
        self.assertEqual(lockpath.stat().st_ino, inode)

    def test_process_arguments_protect_absolute_and_incremental_targets(self):
        output = f'123 /toolchain/bin/rustc -C incremental={self.root}/target/debug/incremental --out-dir {self.root}/target/debug/deps\n124 python3 build.py --output {self.root}/build/live/result.ir\n'
        responses = [subprocess.CompletedProcess([], 0, output, ''), subprocess.CompletedProcess([], 0, '', '')]
        with patch.object(cleanup, 'ROOT', self.root), patch.object(cleanup.subprocess, 'run', side_effect=responses):
            paths = cleanup.active_paths()
        self.assertTrue(cleanup.is_active(self.root / 'target/debug/incremental/crate', paths))
        self.assertTrue(cleanup.is_active(self.root / 'build/live', paths))

    def test_netsurf_experiments_expire_but_native_cache_and_disks_survive(self):
        experiment = self.file('build/netsurf/decode-pass/browser/native/output.ir')
        source = self.file('build/netsurf/decode-pass/probe.c')
        disk = self.file('build/netsurf/decode-pass/test.img')
        native = self.file('build/netsurf/native/current.ir')
        portable = self.file('build/netsurf/ripple/old.pobj')
        package = self.file('build/netsurf/netsurf.fxe')
        self.run_cleanup(apply=True)
        self.assertFalse(experiment.exists())
        for p in (source,disk,native,portable,package):self.assertTrue(p.exists(),str(p))
        self.run_cleanup(apply=True,extra=['--deep'])
        self.assertFalse(portable.exists())
        for p in (source,disk,native,package):self.assertTrue(p.exists(),str(p))

    def test_parent_build_argument_protects_nested_netsurf_cache(self):
        output = self.file('build/netsurf/ripple/program.pobj')
        self.run_cleanup(apply=True,active=[self.root/'build/netsurf'],extra=['--deep'])
        self.assertTrue(output.exists())

    def test_netsurf_root_checks_expire_without_touching_browser_or_live_check(self):
        old = self.file('build/netsurf/scrollcheck.fxe')
        live = self.file('build/netsurf/networkcheck.rxe')
        browser = self.file('build/netsurf/netsurf.fxe')
        worker = self.file('build/netsurf/htget.fxe')
        source = self.file('build/netsurf/probe.c')
        self.run_cleanup(apply=True, active=[live])
        self.assertFalse(old.exists())
        for p in (live, browser, worker, source):
            self.assertTrue(p.exists(), str(p))

    def test_dev_cache_requires_opt_in_and_honours_profile_lock(self):
        debug = self.file('target/debug/deps/output.rlib')
        release = self.file('target/release/rcc')
        lockpath = self.file('target/debug/.cargo-lock')
        self.run_cleanup(apply=True)
        self.assertTrue(debug.exists())
        with lockpath.open('a') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
            self.run_cleanup(apply=True,extra=['--dev-cache'])
            self.assertTrue(debug.exists())
        self.run_cleanup(apply=True,extra=['--dev-cache'])
        self.assertFalse(debug.exists())
        self.assertTrue(release.exists())
        self.assertTrue(lockpath.exists())

    def test_qemu_images_protect_active_build(self):
        disk=self.file('build/x64/disk.img')
        output=f'123 /opt/bin/qemu-system-x86_64 -drive file={disk}\n'
        responses=[subprocess.CompletedProcess([],0,output,''),subprocess.CompletedProcess([],0,'','')]
        with patch.object(cleanup,'ROOT',self.root),patch.object(cleanup.subprocess,'run',side_effect=responses):
            paths=cleanup.active_paths()
        self.assertTrue(cleanup.is_active(disk.parent,paths))

    def test_long_running_default_sdk_job_preserves_outputs_between_phases(self):
        self.file('build/portable-native-sdk/guest/runtime.o')
        self.file('build/guest-native-assessment/qbe.fxe')
        self.file('rcc-portable/build/apps/rcc.fxe')
        responses=[subprocess.CompletedProcess([],0,'123 python3 rcc-portable/native-sdk/test.py --timeout 7200\n',''),
                   subprocess.CompletedProcess([],0,'','')]
        with patch.object(cleanup,'ROOT',self.root),patch.object(cleanup.subprocess,'run',side_effect=responses):
            paths=cleanup.active_paths()
        for name in ('build/portable-native-sdk','build/guest-native-assessment','rcc-portable/build'):
            self.assertTrue(cleanup.is_active(self.root/name,paths),name)

    def test_allocation_estimate_counts_only_fully_removed_hardlinks(self):
        a=self.file('build/test/a.ir');b=a.with_name('b.ir');os.link(a,b)
        self.assertEqual(cleanup.allocated_bytes([(a,a.stat())]),0)
        self.assertEqual(cleanup.allocated_bytes([(a,a.stat()),(b,b.stat())]),a.stat().st_blocks*512)

    def test_legacy_backups_share_retention_and_live_guest_disk_is_untouched(self):
        disk=self.file('build/fuzix/root.img')
        old=self.file('build/fuzix/root.img.editor-updates.bak')
        newer=self.file('build/fuzix/root.img.bak')
        os.utime(newer,(self.age+1,self.age+1))
        self.run_cleanup(apply=True,active=[disk])
        self.assertFalse(old.exists())
        self.assertTrue(newer.exists())
        self.assertTrue(disk.exists())
        self.run_cleanup(apply=True,active=[newer],extra=['--keep-backups','0'])
        self.assertTrue(newer.exists(),'an open backup must never be removed')
        lock=self.file('build/fuzix/.build.lock')
        self.run_cleanup(apply=True,active=[lock],extra=['--keep-backups','0'])
        self.assertTrue(newer.exists(),'an active image builder protects backups')


if __name__ == '__main__':
    unittest.main()
