#!/usr/bin/env python3
"""Run one bounded browser scanner test/fixture on the host and a fresh C16 guest."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
INPUTS = ('ports/lvgl/browser.c', 'ports/lvgl/browser.h',
          'ports/lvgl/browser-scan-test.c', 'ports/lvgl/browser-scan-fixtures.py',
          'ports/lvgl/browser-scan-test.py')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def make_image(directory, program, fixtures):
    """The ordinary Guest helper has 256 inodes; the 1024-entry fixture needs more.

    Build an explicit disposable image with the same small shell/runtime base,
    then ask Guest to boot it with reuse=True. No existing disk is opened.
    """
    system = ROOT / 'fuse-x/build'
    files = [dict(name='/bin/'+name, mode=0o100755,
                  path=str(system/'apps'/(name+'.fxe'))) for name in ('sh', 'echo', 'cat')]
    files += [dict(name='/bin/browser-scan', mode=0o100755, path=str(program))]
    files += [dict(name='/lib/fuse-x/'+p.name, mode=0o100644, path=str(p))
              for p in sorted((system/'lib').glob('*.fxl.[0-9]'))]
    files += list(fixtures)
    files += [dict(name='/init', mode=0o100755, text='FXN1:init:1\n'),
              dict(name='/dev/tty', mode=0o20666, device=0x200),
              dict(name='/dev/console', mode=0o20600, device=0x201),
              dict(name='/dev/null', mode=0o20666, device=0x100),
              dict(name='/dev/urandom', mode=0o20444, device=0x104),
              dict(name='/dev/crypto', mode=0o20666, device=0x105)]
    manifest = directory/'manifest.json'
    manifest.write_text(json.dumps(files))
    builder = r'''
import fs from 'node:fs';
import {rootImage} from './ports/fuzix/tools/disk.mjs';
import {writeImage} from './ports/fuzix/tools/image.mjs';
const [manifest,disk]=process.argv.slice(1);
const files=JSON.parse(fs.readFileSync(manifest,'utf8')).map(f=>({...f,
    data:f.path?fs.readFileSync(f.path):Buffer.from(f.text||'',
        (f.mode&0o170000)===0o120000?'latin1':'utf8')}));
writeImage(disk,rootImage(files,32768,4096));
'''
    subprocess.run(['node', '--input-type=module', '-e', builder,
                    str(manifest), str(directory/'disk.img')], cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT/'build/lvgl/browser-scan-tests')
    parser.add_argument('--compiler', type=Path, default=ROOT/'target/release/rcc',
                        help='frozen C16 compiler used by fuse-x/cc.py')
    parser.add_argument('--sdk-build', type=Path, help='optional isolated native SDK cache')
    args = parser.parse_args()
    out = args.build.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out/'result.json').unlink(missing_ok=True)
    source_hashes = {name: digest(ROOT/name) for name in INPUTS}
    kernel = Path(os.environ.get('FUSE_X_TEST_KERNEL', ROOT/'fuse-x/build/fuse-x')).resolve()
    kernel_hash = digest(kernel)
    compiler = args.compiler.resolve()
    compiler_hash = digest(compiler)

    spec = importlib.util.spec_from_file_location('browser_scan_fixtures', PORT/'browser-scan-fixtures.py')
    fixtures = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixtures)
    folder, manifest = fixtures.prepare(out/'fixtures')
    folder = Path(folder).resolve()
    parameters = [str(folder/'small'), str(folder/'limit')]
    flags = ['-D_POSIX_C_SOURCE=200809L', '-std=c99', '-O1', '-g', '-Wall', '-Wextra',
             '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=all']
    host_cc = 'cc'
    if sys.platform == 'darwin':
        host_cc = '/usr/bin/clang'
        flags += ['-isysroot', '/Applications/Xcode.app/Contents/Developer/Platforms/'
                  'MacOSX.platform/Developer/SDKs/MacOSX15.5.sdk']
    host = out/'browser-scan-host'
    subprocess.run([host_cc, *flags, str(PORT/'browser.c'), str(PORT/'browser-scan-test.c'),
                    '-o', str(host)], cwd=ROOT, check=True)
    result = subprocess.run([str(host), *parameters], cwd=ROOT, env=dict(os.environ, HOME=str(folder)),
                            capture_output=True, text=True, check=True, timeout=120)
    assert 'BROWSER_SCAN passed' in result.stdout, result.stdout+result.stderr
    (out/'host.log').write_text(result.stdout+result.stderr)

    application = out/'browser-scan.fxe'
    command = [sys.executable, str(ROOT/'fuse-x/cc.py'), str(PORT/'browser.c'),
               str(PORT/'browser-scan-test.c'), '--static', '--compiler', str(compiler),
               '--build', str((args.sdk_build or out/'c16').resolve()), '-o', str(application)]
    with (out/'guest-build.log').open('w') as log:
        subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    sys.path.insert(0, str(ROOT/'ports/tls/tests'))
    from guest import Guest
    with tempfile.TemporaryDirectory(prefix='graphite-browser-scan-') as temporary:
        directory = Path(temporary)
        make_image(directory, application, manifest)
        machine = Guest(directory, 'native', {'browser-scan': application}, reuse=True)
        try:
            output = machine.shell('browser-scan '+shlex.join(parameters)+'; echo BROWSER_SCAN_STATUS:$?', seconds=240)
            assert 'BROWSER_SCAN passed' in output and 'BROWSER_SCAN_STATUS:0' in output, output
        finally:
            machine.close()
            (out/'guest.log').write_bytes(machine.log)
    assert digest(kernel) == kernel_hash, 'Kernel changed while testing'
    assert digest(compiler) == compiler_hash, 'Compiler changed while testing'
    assert {name: digest(ROOT/name) for name in INPUTS} == source_hashes, 'Scanner test inputs changed'
    report = dict(status='passed', host_status='passed', guest_status='passed', sanitizer='undefined',
                  kernel=str(kernel), kernel_sha256=kernel_hash, compiler_sha256=compiler_hash,
                  executable_sha256=digest(application), input_sha256=source_hashes,
                  fixture_count=len(manifest), fresh_explicit_manifest=True, inode_count=4096,
                  checks=['same scanner test and synthetic fixture in host and C16 guest',
                          'bounded incremental scan/sort and cancellation',
                          'physical symlink resolution and unchanged caller cwd',
                          'transactional failure and directory listing limits',
                          'UTF-8 names, hidden entries and directory-first ordering'])
    (out/'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print('Browser scanner: host UBSan and fresh C16 guest checks passed', flush=True)


if __name__ == '__main__':
    main()
