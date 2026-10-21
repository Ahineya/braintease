#!/usr/bin/env python3
"""Check that interrupted/stale native source builds cannot claim acceptance."""
import json
import fcntl
from pathlib import Path
import tempfile
import unittest
import subprocess
import sys

from test_make_guest import cached_tool, digest, fingerprint, save_record
from test_tools import discard_guest_disks
from test_x86 import BUILDER, run_guest
from test_extract_guest import CREATE
from extract_guest import extract_files


class Checkpoints(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.object = self.root / 'unit.o'
        self.object.write_bytes(b'guest-object')
        self.program = self.root / 'tool.fxe'
        self.program.write_bytes(b'guest-program')
        self.outputs = ['unit.o', 'tool.fxe']
        self.record = dict(status='passed', key='inputs', host_link_parity=True,
                           outputs={n: digest(self.root / n) for n in self.outputs})
        save_record(self.root / 'result.json', self.record)

    def test_completed_outputs_reused(self):
        self.assertEqual(cached_tool(self.root, 'inputs', self.outputs), self.record)

    def test_changed_inputs_rejected(self):
        self.assertIsNone(cached_tool(self.root, 'new-inputs', self.outputs))

    def test_changed_or_missing_output_rejected(self):
        for path in [self.object, self.program]:
            original = path.read_bytes()
            path.write_bytes(b'changed')
            self.assertIsNone(cached_tool(self.root, 'inputs', self.outputs))
            path.unlink()
            self.assertIsNone(cached_tool(self.root, 'inputs', self.outputs))
            path.write_bytes(original)

    def test_partial_output_set_rejected(self):
        self.assertIsNone(cached_tool(self.root, 'inputs', ['tool.fxe']))
        self.assertIsNone(cached_tool(self.root, 'inputs', self.outputs + ['absent.o']))

    def test_interrupted_record_rejected(self):
        record = self.root / 'result.json'
        for text in ['{', 'null', '[]', '{}',
                     json.dumps(dict(self.record, status='building')),
                     json.dumps(dict(self.record, host_link_parity=False)),
                     json.dumps(dict(self.record, outputs=self.outputs))]:
            record.write_text(text)
            self.assertIsNone(cached_tool(self.root, 'inputs', self.outputs))
        record.unlink()
        (self.root / 'result.tmp').write_text(json.dumps(self.record))
        self.assertIsNone(cached_tool(self.root, 'inputs', self.outputs))

    def test_fingerprint_covers_all_guest_inputs(self):
        image = self.root / 'image'
        image.write_bytes(b'kernel')
        files = [dict(name='/src.c', path=str(self.object), mode=0o100644),
                 dict(name='/bin/tool', path=str(self.program), mode=0o100755)]
        rows = [dict(source='src.c', tool='tool')]
        baseline = fingerprint(files, [image], rows)
        self.assertEqual(baseline, fingerprint(list(reversed(files)), [image], rows))
        self.program.write_bytes(b'new-seed')
        self.assertNotEqual(baseline, fingerprint(files, [image], rows))
        self.program.write_bytes(b'guest-program')
        image.write_bytes(b'new-kernel')
        self.assertNotEqual(baseline, fingerprint(files, [image], rows))
        image.write_bytes(b'kernel')
        self.assertNotEqual(baseline, fingerprint(files, [image], rows + [dict(source='extra.c')]))
        renamed = [dict(files[0], name='/other.c'), files[1]]
        self.assertNotEqual(baseline, fingerprint(renamed, [image], rows))
        changed_mode = [dict(files[0], mode=0o100755), files[1]]
        self.assertNotEqual(baseline, fingerprint(changed_mode, [image], rows))

    def test_generations_cannot_overwrite_seeds_or_comparison(self):
        for option in ['--seed-tools', '--compare-tools']:
            result = subprocess.run([sys.executable, str(Path(__file__).with_name('test_make_guest.py')),
                '--sdk', str(self.root), '--source', str(self.root),
                '--results', str(self.root), option, str(self.root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn('separate results directory', result.stderr)
            self.assertEqual(json.loads((self.root / 'result.json').read_text()), self.record)

    def test_cleanup_retains_logs_outputs_and_base_image(self):
        names = ['test-root.img', 'test-vars.fd', 'test.log', 'test-files.json',
                 'tool.fxe', 'other-root.img', 'normal/boot.img', 'normal/root.img']
        for name in names:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'keep')
        discard_guest_disks(self.root, 'test')
        discard_guest_disks(self.root, 'test')
        for name in names:
            self.assertEqual((self.root / name).exists(), name not in names[:2], name)
        with self.assertRaises(ValueError):
            discard_guest_disks(self.root, '../normal')

    def test_duplicate_launcher_cannot_overwrite_active_outputs(self):
        with (self.root / '.native-tool-build.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = subprocess.run([sys.executable, str(Path(__file__).with_name('test_make_guest.py')),
                '--sdk', str(self.root), '--source', str(self.root), '--results', str(self.root),
                '--output', str(self.root)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('already owns this output directory', result.stderr)
            self.assertEqual(json.loads((self.root / 'result.json').read_text()), self.record)

    def test_recovery_cannot_overwrite_source_disk(self):
        source = self.root / 'recovery-root.img'
        source.write_bytes(b'completed-build')
        with self.assertRaisesRegex(ValueError, 'separate'):
            run_guest(self.root, 'recovery', [], [], 1, seed_root=source)
        self.assertEqual(source.read_bytes(), b'completed-build')
        self.assertFalse((self.root / 'recovery-files.json').exists())

    def test_guest_build_disk_has_headroom_and_preserves_seed(self):
        # Generation-two tools and the parser's live IR/assembly exceed the
        # previous 64 MiB image even when temporary cleanup works correctly.
        seed = self.root / 'seed.img'
        output = self.root / 'build-root.img'
        manifest = self.root / 'files.json'
        subprocess.run(['node', '--input-type=module', '-e', CREATE,
                        str(seed), 'clean'], check=True)
        original = digest(seed)
        manifest.write_text(json.dumps([dict(name='/tmp/extra.o',
            path=str(self.object), mode=0o100644)]))
        subprocess.run(['node', '--input-type=module', '-e', BUILDER,
                        str(seed), str(output), str(manifest)], check=True)
        self.assertGreaterEqual(output.stat().st_size, 128 * 1024 * 1024)
        extract_files(output, self.root / 'extracted', {
            'main.o': '/tmp/build/obj/main.o', 'extra.o': '/tmp/extra.o'})
        self.assertEqual((self.root / 'extracted/main.o').read_bytes(), bytes([0,127,128,255]))
        self.assertEqual((self.root / 'extracted/extra.o').read_bytes(), self.object.read_bytes())
        self.assertEqual(digest(seed), original)


if __name__ == '__main__':
    unittest.main()
