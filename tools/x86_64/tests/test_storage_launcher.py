"""Persistent-image creation must never overwrite an existing user image."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor

spec = importlib.util.spec_from_file_location('x64_boot', Path(__file__).resolve().parents[1] / 'boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)


class StorageLauncher(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.seed = self.directory / 'seed.img'
        self.seed.write_bytes(b'RIPPLE-FUZIX-C16-V3' + bytes(16384 - 19))

    def test_seed_once_preserves_changes(self):
        root = self.directory / 'nested' / 'root.img'
        boot.create_root_image(root, self.seed)
        changed = bytearray(root.read_bytes())
        changed[-100:] = b'Z' * 100
        root.write_bytes(changed)
        before = root.stat()
        self.seed.write_bytes(b'RIPPLE-FUZIX-C16-V3' + b'N' * (16384 - 19))
        boot.create_root_image(root, self.seed)
        self.assertEqual(root.read_bytes(), changed)
        self.assertEqual(root.stat().st_ino, before.st_ino)
        self.assertEqual(root.stat().st_mtime_ns, before.st_mtime_ns)

    def test_rejects_non_roots_without_modifying_them(self):
        for content in (b'FAT16' + bytes(16379), b'RIPPLE-FUZIX-C16-V3',
                        self.seed.read_bytes() + b'x'):
            root = self.directory / 'invalid.img'
            root.write_bytes(content)
            with self.assertRaises(RuntimeError):
                boot.create_root_image(root, self.seed)
            self.assertEqual(root.read_bytes(), content)

    def test_rejects_links_directories_and_device_nodes(self):
        link = self.directory / 'link.img'
        link.symlink_to(self.seed)
        with self.assertRaises(RuntimeError):
            boot.create_root_image(link, self.seed)
        self.assertTrue(link.is_symlink())
        with self.assertRaises(RuntimeError):
            boot.validate_root_image(self.directory)
        with self.assertRaises(RuntimeError):
            boot.validate_root_image(Path('/dev/null'))

    def test_concurrent_publication_is_complete(self):
        second = self.directory / 'second.img'
        second.write_bytes(b'RIPPLE-FUZIX-C16-V3' + b'B' * (16384 - 19))
        root = self.directory / 'root.img'
        with ThreadPoolExecutor(max_workers=2) as executor:
            futures = [executor.submit(boot.create_root_image, root, source) for source in (self.seed, second)]
            for future in futures:
                future.result()
        self.assertIn(root.read_bytes(), (self.seed.read_bytes(), second.read_bytes()))
        self.assertFalse(list(self.directory.glob('.riplix-root-*')))


if __name__ == '__main__':
    unittest.main()
