#!/usr/bin/env python3
"""Exercise launcher build decisions without compilers or user disks."""
from contextlib import redirect_stdout
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('build_os', Path(__file__).with_name('build-os.py'))
builder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)


class BuildTests(unittest.TestCase):
    def test_both_launchers_reuse_and_repair_artifacts(self):
        for osname in ['fuzix', 'fuse-x']:
            with self.subTest(os=osname), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                build = root / 'build'
                calls = []

                def write(name, contents='artifact'):
                    path = root / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_text(contents)
                    return path

                source = write('ports/fuzix/ripple/program.c', 'int main(void) { return 0; }')
                telegram = write('ports/telegram/client.c', 'int telegram(void) { return 0; }')
                rust = write('rcc-driver/src/main.rs', 'fn main() {}')
                write('rcc-driver/Cargo.toml', '[package]')
                write('src/ripple-asm/Cargo.toml', '[package]')
                disk = write('build/user.img', 'saved user files')

                def execute(command):
                    calls.append(str(command[0]))
                    if command[0] == 'cargo':
                        for name in ['target/release/rcc', 'target/release/rfl',
                                     'target/release/rvm', 'target/release/librcc_native_runtime.a',
                                     'src/ripple-asm/target/release/rasm',
                                     'src/ripple-asm/target/release/rlink']:
                            write(name, rust.read_text() if name == 'target/release/rcc' else 'artifact')
                    else:
                        write('build/compiler/manifest.json', '[]')
                        write('build/compiler/bin/cc')
                        if osname == 'fuzix':
                            for name in ['FUZIX.SYS', 'bios.bin', '.mmu', '.code-banks', '.bulk-io']:
                                write('build/' + name)
                            write('build/apps.json', '["hello", "telegram"]')
                            write('build/telegram.rxe')
                            write('build/hello.rxe')
                        else:
                            for name in ['fuse-x', 'apps/hello.fxe', 'apps/telegram.fxe', 'apps/music.fxe', 'apps/wavplay.fxe',
                                         'Fuse-X.app/Contents/MacOS/fuse-x',
                                         'Fuse-X.app/Contents/Info.plist']:
                                write('build/' + name)
                            write('build/application-report.json', '[]')

                def invoke(*extra):
                    output = io.StringIO()
                    with patch.object(builder, 'ROOT', root), patch.object(builder, 'run', execute), \
                         patch.object(builder.subprocess, 'check_output', return_value='rustc test version'), \
                         patch('sys.argv', ['build-os.py', osname, '--build', str(build), *extra]), \
                         patch.dict(os.environ), redirect_stdout(output):
                        builder.main()
                    return output.getvalue()

                invoke()
                self.assertEqual(calls, ['cargo', 'cargo', 'make'])
                self.assertIn('current (no compilation needed)', invoke())
                self.assertEqual(len(calls), 3)
                os.utime(source, None)
                invoke()
                self.assertEqual(len(calls), 3, 'timestamps alone must not rebuild')
                source.write_text('int main(void) { return 1; }')
                invoke()
                self.assertEqual(calls[3:], ['make'], 'C edits must reuse host tools')
                rust.write_text('fn main() { println!("changed"); }')
                invoke()
                self.assertEqual(calls[4:], ['cargo', 'cargo', 'make'])
                (build / 'compiler/bin/cc').unlink()
                invoke()
                self.assertEqual(calls[7:], ['make'], 'a missing installed tool must be restaged')
                previous = len(calls)
                telegram.write_text('int telegram(void) { return 1; }')
                invoke()
                self.assertEqual(calls[previous:], ['make'], 'Telegram C edits must rebuild both guests')
                previous = len(calls)
                (build / ('telegram.rxe' if osname == 'fuzix' else 'apps/telegram.fxe')).unlink()
                invoke()
                self.assertEqual(calls[previous:], ['make'], 'missing Telegram must be rebuilt')
                previous = len(calls)
                sdk = write('build/sdk/cached.ir')
                graphics = write('build/nanox-native/cached.ir')
                invoke('--rebuild')
                self.assertEqual(calls[previous:], ['cargo', 'cargo', 'make'])
                self.assertEqual(disk.read_text(), 'saved user files')
                if osname == 'fuse-x':
                    self.assertFalse(sdk.exists())
                    self.assertFalse(graphics.exists())


if __name__ == '__main__':
    unittest.main()
