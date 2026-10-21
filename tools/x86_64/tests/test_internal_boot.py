"""Offline initial EFI staging: no mounted disk or firmware entry is touched."""
import errno
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location('internal_boot', Path(__file__).resolve().parents[1] / 'internal_boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)


def efi_image(marker):
    data = bytearray(1024)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 128)
    data[128:132] = b'PE\0\0'
    struct.pack_into('<HH', data, 132, 0x8664, 1)
    struct.pack_into('<HH', data, 148, 240, 2)
    optional = 152
    struct.pack_into('<H', data, optional, 0x20b)
    struct.pack_into('<I', data, optional + 16, 0x1000)
    struct.pack_into('<II', data, optional + 56, 0x2000, 512)
    struct.pack_into('<H', data, optional + 68, 10)
    section = optional + 240
    data[section:section + 8] = b'.text\0\0\0'
    struct.pack_into('<IIII', data, section + 8, 512, 0x1000, 512, 512)
    struct.pack_into('<I', data, section + 36, 0x60000020)
    data[512:512 + len(marker)] = marker
    return bytes(data)


class InternalBoot(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.esp = self.work / 'esp'
        self.efi = self.esp / 'EFI'
        for relative, data in [('ubuntu/shimx64.efi', b'Ubuntu shim sentinel'),
                               ('ubuntu/grub.cfg', b'Ubuntu config sentinel'),
                               ('BOOT/BOOTX64.EFI', b'existing fallback sentinel'),
                               ('Recovery/BOOTREC.EFI', b'recovery sentinel')]:
            path = self.efi / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (self.esp / 'unrelated.txt').write_bytes(b'root sentinel')
        self.candidate = self.work / 'candidate.efi'
        self.good = self.work / 'accepted-good.efi'
        self.candidate.write_bytes(efi_image(b'candidate'))
        self.good.write_bytes(efi_image(b'previous accepted build'))
        self.candidate_hash = boot.sha256(self.candidate.read_bytes())
        self.good_hash = boot.sha256(self.good.read_bytes())
        self.identity = 'PARTUUID=offline-test-only'
        self.destination = self.efi / 'Riplix'
        self.sentinels = {str(p.relative_to(self.esp)): boot.sha256(p.read_bytes())
                          for p in self.esp.rglob('*') if p.is_file()}

    def apply(self, **kwargs):
        return boot.apply(self.esp, self.candidate, self.candidate_hash,
                          self.good, self.good_hash, self.identity, **kwargs)

    def inspect(self, **kwargs):
        return boot.inspect(self.esp, self.candidate_hash, self.good_hash, self.identity, **kwargs)

    def assert_preserved(self):
        for name, digest in self.sentinels.items():
            self.assertEqual(boot.sha256((self.esp / name).read_bytes()), digest)

    def assert_unpublished(self):
        self.assertFalse(self.destination.exists())
        self.assertEqual(list(self.efi.glob(boot.STAGING_PREFIX + '*')), [])
        self.assert_preserved()

    def test_initial_stage_exact_manifest_and_idempotent_verification(self):
        self.assertEqual(self.inspect()['state'], 'not-staged')
        result = self.apply()
        self.assertEqual(result['action'], 'staged-and-verified')
        self.assertFalse(result['firmware_changed'])
        self.assertEqual(result['state'], 'staged-not-boot-validated')
        self.assertEqual({p.name for p in self.destination.iterdir()}, {'BOOTX64.EFI', 'BOOTGOOD.EFI', 'manifest.json'})
        self.assertEqual((self.destination / 'BOOTX64.EFI').read_bytes(), self.candidate.read_bytes())
        self.assertEqual((self.destination / 'BOOTGOOD.EFI').read_bytes(), self.good.read_bytes())
        self.assertEqual(result['manifest']['caller_verified_esp_identity'], self.identity)
        self.assertEqual(result['manifest']['files']['candidate']['sha256'], self.candidate_hash)
        before = {p.name: p.stat().st_mtime_ns for p in self.destination.iterdir()}
        self.assertEqual(self.apply()['action'], 'already-staged-verified')
        self.assertEqual(before, {p.name: p.stat().st_mtime_ns for p in self.destination.iterdir()})
        self.assertEqual(self.inspect(manifest_hash=result['manifest_sha256'])['manifest_sha256'], result['manifest_sha256'])
        self.assert_preserved()

    def test_reject_bad_source_hash_or_identical_fallback_before_changes(self):
        self.candidate.write_bytes(self.candidate.read_bytes() + b'corruption')
        with self.assertRaisesRegex(boot.StageError, 'SHA256 mismatch'):
            self.apply()
        self.assert_unpublished()
        with self.assertRaisesRegex(boot.StageError, 'distinct'):
            boot.apply(self.esp, self.good, self.good_hash, self.good, self.good_hash, self.identity)
        self.assert_unpublished()

    def test_reject_bad_pe_headers_before_changes(self):
        valid = efi_image(b'valid')
        mutations = [(0, b'XX'), (60, struct.pack('<I', 0xfffffff0)), (128, b'NOPE'),
                     (132, struct.pack('<H', 0x14c)), (152, struct.pack('<H', 0x10b)),
                     (220, struct.pack('<H', 3)), (148, struct.pack('<H', 80)),
                     (168, struct.pack('<I', 0x1800)), (412, struct.pack('<I', 0xffffff00))]
        invalid = [b'', valid[:100], valid[:500]]
        for offset, replacement in mutations:
            data = bytearray(valid); data[offset:offset + len(replacement)] = replacement
            invalid.append(bytes(data))
        for data in invalid:
            with self.subTest(size=len(data), digest=boot.sha256(data)):
                self.candidate.write_bytes(data)
                with self.assertRaises(boot.StageError):
                    boot.apply(self.esp, self.candidate, boot.sha256(data), self.good, self.good_hash, self.identity)
                self.assert_unpublished()

    def test_reject_unexpected_existing_payload_and_case_collision(self):
        self.destination.mkdir()
        foreign = self.destination / 'do-not-replace.efi'
        foreign.write_bytes(b'unexpected existing payload')
        with self.assertRaisesRegex(boot.StageError, 'unexpected'):
            self.apply()
        self.assertEqual(foreign.read_bytes(), b'unexpected existing payload')
        foreign.unlink(); self.destination.rmdir()
        collision = self.efi / 'rIpLiX'; collision.mkdir()
        with self.assertRaisesRegex(boot.StageError, 'case-colliding'):
            self.apply()
        self.assertTrue(collision.is_dir()); self.assert_preserved()

    def test_reject_symlinks_and_nonregular_source(self):
        source = self.work / 'source-link.efi'; source.symlink_to(self.candidate)
        with self.assertRaises(OSError):
            boot.apply(self.esp, source, self.candidate_hash, self.good, self.good_hash, self.identity)
        fifo = self.work / 'fifo.efi'; os.mkfifo(fifo)
        with self.assertRaises(boot.StageError):
            boot.apply(self.esp, fifo, self.candidate_hash, self.good, self.good_hash, self.identity)
        outside = self.work / 'outside'; outside.mkdir()
        self.destination.symlink_to(outside, target_is_directory=True)
        with self.assertRaises(boot.StageError):
            self.apply()
        self.assertEqual(list(outside.iterdir()), []); self.assert_preserved()

    def test_low_space_and_mid_write_failure_leave_no_final_stage(self):
        with mock.patch.object(boot.os, 'statvfs', return_value=SimpleNamespace(f_frsize=4096, f_bavail=1)):
            with self.assertRaisesRegex(boot.StageError, 'insufficient'):
                self.apply()
        self.assert_unpublished()
        original = boot.write_file
        def no_space(path, data):
            if path.name == boot.KNOWN_GOOD:
                path.write_bytes(b'partial')
                raise OSError(errno.ENOSPC, 'injected full filesystem')
            original(path, data)
        with mock.patch.object(boot, 'write_file', side_effect=no_space):
            with self.assertRaises(OSError):
                self.apply()
        self.assert_unpublished()

    def test_readback_corruption_refuses_publication(self):
        original = boot.write_file
        def corrupt(path, data):
            original(path, data)
            if path.name == boot.CANDIDATE:
                path.write_bytes(b'corruption')
        # Corruption after one per-file verification must still be caught by
        # complete-generation verification before final directory publication.
        with mock.patch.object(boot, 'write_file', side_effect=corrupt):
            with self.assertRaises(boot.StageError):
                self.apply()
        self.assert_unpublished()

    def test_post_publication_failure_preserves_evidence(self):
        original = boot.publish_directory
        def corrupt(source, destination):
            original(source, destination)
            (destination / boot.CANDIDATE).write_bytes(b'post-publication corruption')
        with mock.patch.object(boot, 'publish_directory', side_effect=corrupt):
            with self.assertRaises(boot.StageError):
                self.apply()
        self.assertTrue(self.destination.exists())
        with self.assertRaises(boot.StageError):
            self.inspect()
        self.assert_preserved()

    def test_fsync_failure_preserves_published_generation_without_claiming_success(self):
        original = boot.publish_directory
        published = False
        def publish(source, destination):
            nonlocal published
            original(source, destination); published = True
        original_sync = boot.sync_directory
        def sync(path):
            if published:
                raise OSError(errno.EIO, 'injected post-publication fsync failure')
            original_sync(path)
        with mock.patch.object(boot, 'publish_directory', side_effect=publish), \
                mock.patch.object(boot, 'sync_directory', side_effect=sync):
            with self.assertRaises(OSError):
                self.apply()
        self.assertEqual(self.inspect()['state'], 'staged-not-boot-validated')
        with mock.patch.object(boot, 'sync_regular', wraps=boot.sync_regular) as synchronize:
            self.assertEqual(self.apply()['action'], 'already-staged-verified')
            self.assertEqual(synchronize.call_count, 3)
        self.assert_preserved()

    def test_prepublication_fsync_failure_does_not_publish(self):
        original = boot.sync_directory
        def failed(path):
            if path.name.startswith(boot.STAGING_PREFIX):
                raise OSError(errno.EIO, 'injected staging fsync failure')
            original(path)
        with mock.patch.object(boot, 'sync_directory', side_effect=failed):
            with self.assertRaises(OSError):
                self.apply()
        self.assert_unpublished()

    def test_interrupt_leaves_unselected_staging_and_refuses_retry(self):
        original = boot.write_file
        def interrupt(path, data):
            original(path, data)
            if path.name == boot.CANDIDATE:
                raise KeyboardInterrupt('injected power interruption')
        with mock.patch.object(boot, 'write_file', side_effect=interrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.apply()
        result = self.inspect()
        self.assertEqual(result['state'], 'not-staged')
        self.assertEqual(len(result['staging_directories']), 1)
        self.assertFalse(self.destination.exists())
        with self.assertRaisesRegex(boot.StageError, 'interrupted'):
            self.apply()
        self.assert_preserved()

    def test_atomic_publication_refuses_racing_empty_destination(self):
        original = boot.publish_directory
        def racing(source, destination):
            destination.mkdir()
            original(source, destination)
        with mock.patch.object(boot, 'publish_directory', side_effect=racing):
            with self.assertRaises(OSError):
                self.apply()
        self.assertTrue(self.destination.is_dir())
        self.assertEqual(list(self.destination.iterdir()), [])
        self.assertFalse(list(self.efi.glob(boot.STAGING_PREFIX + '*')))
        self.assert_preserved()

    def test_inspection_rejects_corruption_rejected_state_unknown_file_and_identity(self):
        result = self.apply()
        candidate = self.destination / boot.CANDIDATE
        candidate.write_bytes(candidate.read_bytes() + b'corruption')
        with self.assertRaisesRegex(boot.StageError, 'SHA256 mismatch'):
            self.inspect()
        candidate.write_bytes(self.candidate.read_bytes())
        manifest_path = self.destination / boot.MANIFEST
        original = manifest_path.read_bytes()
        manifest = json.loads(original); manifest['state'] = 'rejected'
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(boot.StageError, 'rejected'):
            self.inspect()
        manifest_path.write_bytes(original)
        with self.assertRaisesRegex(boot.StageError, 'manifest SHA256 mismatch'):
            self.inspect(manifest_hash='0' * 64)
        extra = self.destination / 'unexpected'; extra.write_bytes(b'keep me')
        with self.assertRaisesRegex(boot.StageError, 'unexpected'):
            self.inspect()
        self.assertEqual(extra.read_bytes(), b'keep me'); extra.unlink()
        with self.assertRaisesRegex(boot.StageError, 'mismatched'):
            boot.inspect(self.esp, self.candidate_hash, self.good_hash, 'PARTUUID=wrong')
        self.assertEqual(self.inspect()['manifest_sha256'], result['manifest_sha256'])
        self.assert_preserved()

    def test_named_stage_leaves_existing_default_generation_exactly_untouched(self):
        default = self.apply()
        before = {p.name: (boot.sha256(p.read_bytes()), p.stat().st_mtime_ns)
                  for p in self.destination.iterdir()}
        # Distinct inputs ensure named inspection cannot silently inspect Riplix.
        self.candidate.write_bytes(efi_image(b'distinct second namespace trial'))
        self.candidate_hash = boot.sha256(self.candidate.read_bytes())
        self.assertEqual(self.inspect(directory_name='Riplix-tui')['state'], 'not-staged')
        result = self.apply(directory_name='Riplix-tui')
        self.assertEqual(result['action'], 'staged-and-verified')
        self.assertEqual(result['manifest']['files']['candidate']['path'], 'EFI/Riplix-tui/BOOTX64.EFI')
        self.assertEqual(result['manifest']['files']['known_good']['path'], 'EFI/Riplix-tui/BOOTGOOD.EFI')
        self.assertEqual(self.inspect(directory_name='Riplix-tui', manifest_hash=result['manifest_sha256'])
                         ['manifest_sha256'], result['manifest_sha256'])
        self.assertEqual(self.apply(directory_name='Riplix-tui')['action'], 'already-staged-verified')
        self.assertEqual(before, {p.name: (boot.sha256(p.read_bytes()), p.stat().st_mtime_ns)
                                  for p in self.destination.iterdir()})
        old_hash = default['manifest']['files']['candidate']['sha256']
        self.assertEqual(boot.inspect(self.esp, old_hash, self.good_hash, self.identity)
                         ['manifest_sha256'], default['manifest_sha256'])
        self.assert_preserved()

    def test_directory_validation_rejects_traversal_noncanonical_or_long_names_before_changes(self):
        valid = ['Riplix', 'Riplix-tui', 'Riplix-A', 'Riplix-0a-Z', 'Riplix-' + 'A'*48]
        for name in valid:
            self.assertEqual(boot.checked_directory_name(name), name)
        invalid = ['', '.', '..', '../Riplix-tui', '/Riplix-tui', 'Riplix/tui',
                   'Riplix\\tui', 'riplix-tui', 'RIPLIX', 'Riplix-', 'Riplix--tui',
                   'Riplix-.tui', 'Riplix-tui.tmp', 'Riplix-tüi', 'Riplix-a\n',
                   'Riplix-' + 'A'*49, None, 1]
        for name in invalid:
            with self.subTest(name=name):
                with self.assertRaisesRegex(boot.StageError, 'EFI directory must be'):
                    self.apply(directory_name=name)
                with self.assertRaisesRegex(boot.StageError, 'EFI directory must be'):
                    self.inspect(directory_name=name)
                self.assert_unpublished()

    def test_named_destination_rejects_case_collision_and_symlink(self):
        collision = self.efi / 'rIpLiX-tUi'; collision.mkdir()
        with self.assertRaisesRegex(boot.StageError, 'case-colliding'):
            self.apply(directory_name='Riplix-tui')
        self.assertTrue(collision.is_dir()); collision.rmdir()
        outside = self.work / 'outside'; outside.mkdir()
        named = self.efi / 'Riplix-tui'; named.symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(boot.StageError, 'real directory'):
            self.apply(directory_name='Riplix-tui')
        self.assertEqual(list(outside.iterdir()), [])
        self.assert_preserved()

    def test_named_pending_prefix_is_scoped_to_selected_namespace(self):
        default_pending = self.efi / (boot.STAGING_PREFIX + 'interrupted')
        other_pending = self.efi / '.Riplix-other-stage-interrupted'
        default_pending.mkdir(); other_pending.mkdir()
        self.assertEqual(self.inspect(directory_name='Riplix-tui')['staging_directories'], [])
        result = self.apply(directory_name='Riplix-tui')
        self.assertEqual(result['action'], 'staged-and-verified')
        self.assertTrue(default_pending.is_dir() and other_pending.is_dir())
        own_pending = self.efi / '.rIpLiX-tUi-StAgE-interrupted'; own_pending.mkdir()
        self.assertEqual(self.inspect(directory_name='Riplix-tui')['staging_directories'], [own_pending.name])
        with self.assertRaisesRegex(boot.StageError, 'interrupted'):
            self.apply(directory_name='Riplix-tui')
        with self.assertRaisesRegex(boot.StageError, 'interrupted'):
            self.apply()
        self.assert_preserved()

    def test_named_manifest_rejects_paths_for_another_namespace(self):
        self.apply(directory_name='Riplix-tui')
        path = self.efi / 'Riplix-tui' / boot.MANIFEST
        manifest = json.loads(path.read_bytes())
        manifest['files']['candidate']['path'] = 'EFI/Riplix/BOOTX64.EFI'
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(boot.StageError, 'exact candidate bytes'):
            self.inspect(directory_name='Riplix-tui')
        self.assertFalse(self.destination.exists()); self.assert_preserved()

    def test_cli_threads_explicit_directory_through_apply_and_inspect(self):
        arguments = ['--esp-dir', str(self.esp), '--esp-id', self.identity,
                     '--candidate-sha256', self.candidate_hash, '--known-good-sha256', self.good_hash,
                     '--directory', 'Riplix-tui']
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(boot.main(['apply', *arguments, '--candidate', str(self.candidate),
                                        '--known-good', str(self.good)]), 0)
        result = json.loads(output.getvalue())
        self.assertEqual(result['manifest']['files']['candidate']['path'], 'EFI/Riplix-tui/BOOTX64.EFI')
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(boot.main(['inspect', *arguments, '--manifest-sha256', result['manifest_sha256']]), 0)
        self.assertEqual(json.loads(output.getvalue())['manifest_sha256'], result['manifest_sha256'])
        self.assertFalse(self.destination.exists()); self.assert_preserved()


if __name__ == '__main__':
    unittest.main()
