"""Recovery cannot silently mix sources, SDKs, seeds or object versions."""
import json
from pathlib import Path
import tempfile
import unittest

from reuse_objects import prepare_reuse
from test_make_guest import digest
from test_x86 import file


class RecoveredObjects(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.results = self.root / 'results'
        self.results.mkdir()
        (self.root / 'partial-objects').mkdir()
        (self.root / 'partial-objects/a.o').write_bytes(b'completed guest object')
        self.make = self.source / 'Makefile'
        self.make.write_text('all: build/bin/tool\n'
            'build/obj/a.o: src/a.c header seed\n\tmkdir -p build/obj\n\tcc a.c\n'
            'build/obj/b.o: src/b.c header seed\n\tcc b.c\n'
            'build/bin/tool: build/obj/a.o build/obj/b.o\n\tlink\n')
        self.files = [file('/tmp/native-tools/Makefile', self.make)]
        for i, name in enumerate(['/tmp/native-tools/src/a.c', '/tmp/sdk/libc.a',
                                  '/bin/rcc-c', '/bin/qbe', '/bin/rcc-x64-as',
                                  '/bin/rcc-qbe', '/bin/rcc-x64-ld']):
            path = self.root / str(i)
            path.write_bytes(name.encode())
            self.files.append(file(name, path))
        self.rows = [dict(source='src/' + n + '.c', object='build/obj/' + n + '.o')
                     for n in ['a', 'b']]
        self.record = dict(status='verified-recovered-inputs', original_disk_sha256='disk',
            previous_key='original-failed-build',
            inputs={f['name']: digest(Path(f['path'])) for f in self.files},
            objects={'a.o': digest(self.root / 'partial-objects/a.o')})
        self.path = self.root / 'reuse.json'

    def prepare(self):
        self.path.write_text(json.dumps(self.record))
        return prepare_reuse(self.path, self.source, self.rows, self.files, self.results)

    def test_reuses_only_verified_objects_and_retains_other_rules(self):
        files, report = self.prepare()
        text = (self.results / 'resume.Makefile').read_text()
        self.assertIn('build/obj/a.o:\n', text)
        self.assertNotIn('\tcc a.c', text)
        self.assertIn('build/obj/b.o: src/b.c header seed\n\tcc b.c', text)
        self.assertIn('build/bin/tool: build/obj/a.o build/obj/b.o\n\tlink', text)
        self.assertEqual(report['newly_compiled_units'], 1)
        self.assertEqual(files[-1]['name'], '/tmp/native-tools/build/obj/a.o')

    def test_only_lowerer_and_linker_may_change(self):
        for f in self.files:
            path = Path(f['path'])
            original = path.read_bytes()
            path.write_bytes(original + b'changed')
            if f['name'] in ['/bin/rcc-qbe', '/bin/rcc-x64-ld']:
                _, report = self.prepare()
                self.assertIn(f['name'], report['replaced_seeds'])
            else:
                with self.assertRaises(ValueError):
                    self.prepare()
            path.write_bytes(original)

    def test_changed_object_rejected(self):
        (self.root / 'partial-objects/a.o').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'changed recovered object'):
            self.prepare()

    def test_missing_or_extra_input_rejected(self):
        old = dict(self.record['inputs'])
        for inputs in [{}, dict(old, extra='extra')]:
            self.record['inputs'] = inputs
            with self.assertRaises(ValueError):
                self.prepare()

    def test_unknown_or_empty_object_set_rejected(self):
        for objects in [{}, {'absent.o': 'hash'}, {'../a.o': 'hash'}]:
            self.record['objects'] = objects
            with self.assertRaises(ValueError):
                self.prepare()

    def test_unverified_recovery_rejected(self):
        self.record['status'] = 'partial-build-extracted'
        with self.assertRaises(ValueError):
            self.prepare()


if __name__ == '__main__':
    unittest.main()
