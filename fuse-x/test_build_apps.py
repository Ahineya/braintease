#!/usr/bin/env python3
"""Exercise application cache/retry decisions with tiny fake compilers."""
from contextlib import redirect_stdout, redirect_stderr
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('fuse_apps', Path(__file__).with_name('apps.py'))
apps = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(apps)


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.here = self.root / 'fuse-x'
        self.port = self.root / 'ports/fuzix'
        self.build = self.here / 'build'
        self.source = self.write('ports/program.c', 'int main(void) {return 0;}')
        self.compiler = self.write('target/release/rcc')
        self.write('target/release/rcpp')
        self.write('target/release/librcc_native_runtime.a')
        for path in ['fuse-x/cc.py', 'fuse-x/libc/bridge.c', 'fuse-x/include/memory-layout.h', 'ports/fuzix/ripple/userlib.c',
                     'ports/fuzix/upstream/Kernel/include/userstructs.h']:
            self.write(path)
        self.write('ports/lynx/build.py', '''import pathlib, sys
output = pathlib.Path(sys.argv[sys.argv.index('-o') + 1])
count = output.parent / 'attempts'
count.write_text(str(int(count.read_text()) + 1) if count.exists() else '1')
if (output.parent / 'fail').exists():
    print('compile error')
    sys.exit(1)
output.write_text('executable')
''')

    def write(self, name, text='fixture'):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def invoke(self, *arguments):
        output = io.StringIO()
        with patch.object(apps, 'HERE', self.here), patch.object(apps, 'PORT', self.port), \
             patch.object(apps, 'recipe', return_value=([self.source], [])), \
             patch.object(apps, 'names', return_value=['lynx']), \
             patch.dict(os.environ, {'RIPPLE_REBUILD': '0'}), \
             patch('sys.argv', ['apps.py', '--compiler', str(self.compiler),
                                '--build', str(self.build), *arguments]), \
             redirect_stdout(output), redirect_stderr(output):
            result = apps.main()
        return result, output.getvalue()

    def attempts(self):
        return int((self.build / 'apps/attempts').read_text())

    def test_named_then_inventory_reuses_success_and_repairs_missing_output(self):
        self.assertEqual(self.invoke('lynx')[0], 0)
        code, output = self.invoke('--all', '--require', 'lynx')
        self.assertEqual(code, 0)
        self.assertIn('lynx: built (cached)', output)
        self.assertEqual(self.attempts(), 1)
        (self.build / 'apps/lynx.fxe').unlink()
        self.assertEqual(self.invoke('--all', '--require', 'lynx')[0], 0)
        self.assertEqual(self.attempts(), 2)
        self.source.write_text('changed source')
        self.invoke('lynx')
        self.assertEqual(self.attempts(), 3)

    def test_failed_build_is_retryable_and_required_inventory_fails(self):
        self.invoke('lynx')
        (self.build / 'apps/fail').touch()
        self.source.write_text('rebuild')
        self.assertEqual(self.invoke('--all', '--require', 'lynx')[0], 1)
        self.assertFalse((self.build / 'apps/lynx.fxe').exists())
        self.assertEqual(self.invoke('lynx')[0], 1)
        self.assertEqual(self.attempts(), 3)
        (self.build / 'apps/fail').unlink()
        self.assertEqual(self.invoke('lynx')[0], 0)
        self.assertEqual(self.attempts(), 4)

    def test_default_make_graph_only_builds_lynx_in_inventory(self):
        text = Path(__file__).with_name('Makefile').read_text()
        names = text.split('APP_NAMES :=', 1)[1].splitlines()[0].split()
        self.assertNotIn('lynx', names)
        self.assertIn('apps.py --all --require lynx', text)
        self.assertIn('--require lynx --require chat', text)
        self.assertIn('$(BUILD)/apps/lynx.fxe: lynx-check', text)

    def test_chat_inventory_handles_sources_outside_fuzix_port(self):
        self.source = self.write('ports/chat/main.c', 'int main(void) {return 0;}')
        self.write('ports/chat/build.py', (self.root / 'ports/lynx/build.py').read_text())
        self.assertEqual(self.invoke('chat')[0], 0)
        record = json.loads((self.build / 'application-logs/chat.json').read_text())
        self.assertEqual(record['status'], 'built')
        self.assertEqual(record['sources'], ['chat/main.c'])
        self.assertTrue((self.build / 'apps/chat.fxe').is_file())
        code, output = self.invoke('chat')
        self.assertEqual(code, 0)
        self.assertIn('chat: built (cached)', output)
        self.assertEqual(self.attempts(), 1)


if __name__ == '__main__':
    unittest.main()
