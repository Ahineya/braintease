#!/usr/bin/env python3
"""Check Graphite cache invalidation and the real read-only catalog manifest."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('graphite_build', ROOT / 'fuse-x/graphite.py')
graphite = importlib.util.module_from_spec(spec)
spec.loader.exec_module(graphite)


class GraphiteIntegration(unittest.TestCase):
    def test_build_cache_tracks_abi_libraries_and_force(self):
        with tempfile.TemporaryDirectory(prefix='graphite-build-cache-') as folder:
            root = Path(folder)
            build = root / 'output'
            compiler = root / 'compiler'
            compiler.write_text('compiler version one')
            sources = [
                'ports/lvgl/calculator.c',
                'ports/lvgl/build.py',
                'ports/fusede/catalog/calculator.ini',
                'ports/fuzix/ripple/libc/heap.c',
                'ports/fuzix/upstream/Library/libs/strlen.c',
                'ports/fuzix/upstream/Kernel/include/userstructs.h',
                'runtime/include/ripple.h',
                'libraries/text/text.c',
                'libraries/unicode/unicode.c',
            ]
            for relative in sources:
                file = root / relative
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text('original ' + relative)
            calls = []

            def fake_builder(command, **kwargs):
                calls.append(command)
                product = Path(command[command.index('--build') + 1])
                product.mkdir(parents=True, exist_ok=True)
                app = command[command.index('--app') + 1]
                names = graphite.APPLICATIONS[:-1] if app == 'all' else [app]
                for name in names:
                    (product / (name + '.fxe')).write_text(f'{name} generation {len(calls)}')
                return subprocess.CompletedProcess(command, 0)

            def run():
                with mock.patch.object(sys, 'argv', ['graphite.py', '--build', str(build), '--compiler', str(compiler)]):
                    graphite.main()

            environment = {'RCC_NATIVE_MEMORY': 'checked', 'FUSE_X_HEAP_PROFILE': 'hosted', 'RIPPLE_REBUILD': ''}
            with mock.patch.object(graphite, 'ROOT', root), mock.patch.object(graphite.subprocess, 'run', side_effect=fake_builder), mock.patch.dict(os.environ, environment):
                run()
                self.assertEqual(len(calls), 2)
                run()
                self.assertEqual(len(calls), 2, 'unchanged cache must skip both builders')
                for relative in sources:
                    with self.subTest(source=relative):
                        before = len(calls)
                        (root / relative).write_text('changed ' + relative)
                        run()
                        self.assertEqual(len(calls), before + 2)
                for key, value in [('RCC_NATIVE_MEMORY', 'read-v1'), ('FUSE_X_HEAP_PROFILE', 'bounded')]:
                    with self.subTest(setting=key):
                        before = len(calls)
                        os.environ[key] = value
                        run()
                        self.assertEqual(len(calls), before + 2)
                        run()
                        self.assertEqual(len(calls), before + 2)
                before = len(calls)
                os.environ['RIPPLE_REBUILD'] = '1'
                run()
                self.assertEqual(len(calls), before + 2)
                os.environ['RIPPLE_REBUILD'] = ''
                before = len(calls)
                (build / 'apps/lvcalc.fxe').unlink()
                run()
                self.assertEqual(len(calls), before + 2, 'deleted installed output must be repaired')
                before = len(calls)
                (build / 'apps/lvcalc.fxe').write_text('corrupted')
                run()
                self.assertEqual(len(calls), before + 2, 'modified installed output must be repaired')

    def test_catalog_install_manifest_contains_only_system_application_files(self):
        module = (ROOT / 'ports/fusede/catalog/files.mjs').as_uri()
        code = f"import {{graphiteCatalogFiles}} from {json.dumps(module)}; console.log(JSON.stringify(graphiteCatalogFiles().map(f=>({{name:f.name,mode:f.mode,data:f.data.toString('utf8')}}))));"
        files = json.loads(subprocess.check_output(['node', '--input-type=module', '-e', code], text=True))
        checked_in = sorted((ROOT / 'ports/fusede/catalog').glob('*.ini'))
        self.assertEqual([file['name'] for file in files], ['/usr/share/applications/' + file.name for file in checked_in])
        self.assertEqual(len(files), 13)
        self.assertEqual(len({file['name'] for file in files}), len(files))
        for item, source in zip(files, checked_in):
            self.assertEqual(item['mode'], 0o100644)
            self.assertEqual(item['data'], source.read_text())
            self.assertTrue(item['name'].endswith('.ini'))
            self.assertNotIn('pins', item['name'])


if __name__ == '__main__':
    unittest.main()
