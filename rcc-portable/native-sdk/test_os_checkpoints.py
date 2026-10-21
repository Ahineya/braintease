"""A partial OS build cannot reuse changed frontend inputs or reference IR."""
import copy
import json
from pathlib import Path
import tempfile
import unittest

from source_os import digest, portable_options, compile_command, ROOT
from test_os_guest import frontend_checkpoint, reference_outputs
from platform_capsule import OBJECTS, freeze, validate
from test_os_kernel import lowering_checkpoint


class OsCheckpoints(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'u0.ir').write_bytes(b'compiled guest IR')
        self.manifest = dict(rows=[dict(output='ir/u0.ir')])
        self.inputs = dict(frontend_sha256='frontend', manifest_sha256='manifest',
                           build_inputs=dict(kernel='kernel', make='make'))
        self.previous = dict(**self.inputs, host_frontend_parity=True, guest_commands=['compile'],
                             ir_sha256={'u0.ir': digest(self.root / 'u0.ir')})

    def test_verified_frontend_reused(self):
        self.assertEqual(frontend_checkpoint(self.previous, self.inputs, self.manifest, self.root), ['compile'])

    def test_changed_seeds_sources_or_base_rejected(self):
        for key in self.inputs:
            changed = copy.deepcopy(self.inputs)
            changed[key] = 'changed'
            with self.assertRaises(ValueError):
                frontend_checkpoint(self.previous, changed, self.manifest, self.root)

    def test_partial_or_modified_outputs_rejected(self):
        for hashes in [{}, dict(self.previous['ir_sha256'], extra='extra'), {'u0.ir': 'changed'}]:
            record = dict(self.previous, ir_sha256=hashes)
            with self.assertRaises(ValueError):
                frontend_checkpoint(record, self.inputs, self.manifest, self.root)
        (self.root / 'u0.ir').write_bytes(b'modified')
        with self.assertRaises(ValueError):
            frontend_checkpoint(self.previous, self.inputs, self.manifest, self.root)

    def test_unverified_parity_rejected(self):
        with self.assertRaises(ValueError):
            frontend_checkpoint(dict(self.previous, host_frontend_parity=False),
                                self.inputs, self.manifest, self.root)

    def test_reference_outputs_are_fingerprinted(self):
        (self.root / 'manifest.json').write_text(json.dumps(self.manifest))
        (self.root / 'host-ir').mkdir()
        reference = self.root / 'host-ir/u0.ir'
        reference.write_bytes(b'reference')
        assessment = dict(status='passed', manifest_sha256=digest(self.root / 'manifest.json'),
                          units=[dict(output='ir/u0.ir', sha256=digest(reference))])
        (self.root / 'assessment.json').write_text(json.dumps(assessment))
        self.assertEqual(reference_outputs(self.root, self.manifest), assessment)
        reference.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'reference IR changed'):
            reference_outputs(self.root, self.manifest)

    def test_unknown_flags_and_bank_layouts_rejected(self):
        for flags in [['-O3'], ['--bank-size', '32000'], ['--target', 'ripple']]:
            with self.assertRaises(ValueError):
                portable_options(flags, ROOT)
        self.assertEqual(portable_options(['--bank-size', '64000', '--target', 'native'], ROOT),
                         ['--target', 'native'])
        self.assertIsNone(compile_command('mkdir -p build', ROOT / 'target/release/rcc'))

    def test_platform_capsule_excludes_kernel_and_rejects_changed_objects(self):
        base = self.root / 'base'
        base.mkdir()
        for name in OBJECTS | {'loader.obj', 'root.img', 'build.json', 'rcc.o'}:
            (base / name).write_bytes(name.encode())
        output = self.root / 'capsule'
        record = freeze(base, output)
        self.assertFalse((output / 'rcc.o').exists())
        self.assertFalse(record['guest_compiled'])
        self.assertEqual(len(validate(output)), len(OBJECTS))
        with self.assertRaises(ValueError):
            freeze(base, output)
        (output / 'timer.o').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'content changed'):
            validate(output)

    def test_different_platform_profile_rejected(self):
        base = self.root / 'base'
        base.mkdir()
        for name in OBJECTS:
            (base / name).write_bytes(name.encode())
        (base / 'wifi.o').write_bytes(b'another profile')
        with self.assertRaises(ValueError):
            validate(base)

    def test_combined_kernel_lowering_checkpoint(self):
        artifact = self.root / 'kernel.ssa'
        artifact.write_bytes(b'combined kernel QBE')
        state = dict(source_manifest_sha256='source', os_result_sha256='os', build_base={'kernel': 'base'},
                     guest_tools={'rcc-qbe': {'path': 'new/path', 'sha256': 'lowerer'}})
        previous = dict(state, status='passed', lowerer_parity=True, lowering=['command'],
                        outputs={'kernel.ssa': digest(artifact)})
        previous['guest_tools'] = {'rcc-qbe': {'path': 'old/path', 'sha256': 'lowerer'}}
        self.assertEqual(lowering_checkpoint(previous, state, artifact), ['command'])
        for key in ['source_manifest_sha256', 'os_result_sha256', 'build_base']:
            changed = dict(state, **{key: 'changed'})
            with self.assertRaises(ValueError):
                lowering_checkpoint(previous, changed, artifact)
        for changed in [dict(previous, status='failed'), dict(previous, lowerer_parity=False),
                        dict(previous, outputs={}), dict(previous, guest_tools={})]:
            with self.assertRaises(ValueError):
                lowering_checkpoint(changed, state, artifact)
        artifact.write_bytes(b'modified')
        with self.assertRaises(ValueError):
            lowering_checkpoint(previous, state, artifact)


if __name__ == '__main__':
    unittest.main()
