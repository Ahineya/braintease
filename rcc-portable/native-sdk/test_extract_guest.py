#!/usr/bin/env python3
"""Real filesystem extraction: nested paths, rejected data and unchanged disks."""
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest
from extract_guest import ROOT, extract_files

CREATE = r'''
import {rootImage} from './ports/fuzix/tools/disk.mjs';
import {writeImage} from './ports/fuzix/tools/image.mjs';
import {CELLS,SB} from './ports/fuzix/tools/layout.mjs';
const [output,mode]=process.argv.slice(1);
const image=rootImage([
 {name:'/tmp/build/obj/main.o',data:Buffer.from([0,127,128,255])},
 {name:'/tmp/build/bin/program',data:Buffer.from('program')},
 {name:'/tmp/wide',data:[256]},
 {name:'/tmp/link',symlink:'/tmp/build/bin/program'}],256);
if(mode==='dirty')image.writeUInt16LE(1,(CELLS+SB.clean)*2);
writeImage(output,image);
'''


class Extract(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = Path(self.folder.name)
        self.disk = self.root / 'disk.img'
        self.output = self.root / 'out'
        subprocess.run(['node', '--input-type=module', '-e', CREATE, str(self.disk), 'clean'],
                       cwd=ROOT, check=True)
        self.before = hashlib.sha256(self.disk.read_bytes()).hexdigest()

    def tearDown(self):
        self.assertEqual(hashlib.sha256(self.disk.read_bytes()).hexdigest(), self.before)

    def test_nested_files_without_guest_copy(self):
        extract_files(self.disk, self.output, {'main.o': '/tmp/build/obj/main.o',
                                             'program.fxe': '/tmp/build/bin/program'})
        self.assertEqual((self.output / 'main.o').read_bytes(), bytes([0,127,128,255]))
        self.assertEqual((self.output / 'program.fxe').read_bytes(), b'program')

    def test_invalid_paths_cannot_escape(self):
        for name, path in [('../escape', '/tmp/wide'), ('../', '/tmp/wide'),
                           ('out', '/tmp/../wide'), ('out', '/tmp//wide'),
                           ('out', 'tmp/wide'), ('out', '/tmp/./wide')]:
            with self.assertRaises(ValueError):
                extract_files(self.disk, self.output, {name:path})

    def test_all_files_validated_before_existing_outputs_change(self):
        self.output.mkdir()
        (self.output / 'keep').write_bytes(b'preserved')
        for path in ['/tmp/wide', '/tmp/missing', '/tmp/build', '/tmp/link',
                     '/tmp/build/bin/program/child']:
            with self.assertRaises(subprocess.CalledProcessError):
                extract_files(self.disk, self.output,
                              {'keep':'/tmp/build/bin/program', 'bad':path})
            self.assertEqual((self.output / 'keep').read_bytes(), b'preserved')
            self.assertFalse((self.output / 'bad').exists())

    def test_unclean_filesystem_rejected(self):
        subprocess.run(['node', '--input-type=module', '-e', CREATE, str(self.disk), 'dirty'],
                       cwd=ROOT, check=True)
        self.before = hashlib.sha256(self.disk.read_bytes()).hexdigest()
        with self.assertRaises(subprocess.CalledProcessError):
            extract_files(self.disk, self.output, {'program':'/tmp/build/bin/program'})


if __name__ == '__main__':
    unittest.main()
