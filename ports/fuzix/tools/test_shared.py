#!/usr/bin/env python3
"""Exercise RXE2 libraries on a disposable real RVM guest, optionally with TLS."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import statistics
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest
from shared import format as fmt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/riplix-shared')
    parser.add_argument('--skip-build', action='store_true')
    parser.add_argument('--tls', action='store_true', help='also check shared TLS against an OpenSSL Ethernet peer')
    parser.add_argument('--benchmark', action='store_true', help='measure identical-object static/shared cat launches')
    args = parser.parse_args()
    build = args.build.resolve()
    env = dict(os.environ, RIPPLE_IMAGE_BACKUPS='0', RIPLIX_SHARED_LIBRARIES='1')

    def run(*command, **kwargs):
        return subprocess.run(list(map(str, command)), cwd=ROOT, env=env, check=True, **kwargs)

    if not args.skip_build:
        names = ['init', 'sh', 'echo', 'cat', 'cursescheck', 'rfl-demo']
        if args.tls:
            names += ['htget', 'cmp']
        run('make', '-C', ROOT / 'ports/fuzix', f'BUILD={build}', '-j4', 'kernel', 'bios',
            *[build / (name + '.rxe') for name in names])
    fixture = importlib.util.spec_from_file_location('native_shared_fixture', ROOT / 'fuse-x/test_shared_libraries.py')
    module = importlib.util.module_from_spec(fixture)
    fixture.loader.exec_module(module)
    with tempfile.TemporaryDirectory(prefix='riplix-shared-') as temporary:
        directory = Path(temporary)
        library_source = directory / 'library.c'
        library_source.write_text(module.LIBRARY + '''
char shared_large[66000];
int (*shared_identity(void))(int (*)(int)) {return shared_callback;}
int shared_large_check(void) {return shared_large[65000]+shared_large[65999];}
''')
        source = '#include <signal.h>\n#include <setjmp.h>\n'+module.APPLICATION
        source = source.replace('int main(int argc,char **argv) {', '''
extern char shared_large[66000];
extern int (*shared_identity(void))(int (*)(int));
extern int shared_large_check(void);
static char *large_import=shared_large+65000;
static jmp_buf shared_jump;
static volatile sig_atomic_t caught;
static void catch_signal(int sig) {caught=sig;}
int main(int argc,char **argv) {''')
        source = source.replace('CHECK(shared_counter==7 && shared_pointer[0]', '''
    CHECK(shared_identity()==shared_callback);
    CHECK(signal(SIGUSR1,catch_signal)!=SIG_ERR);
    CHECK(kill(getpid(),SIGUSR1)==0 && caught==SIGUSR1);
    {int value=setjmp(shared_jump);if(!value)longjmp(shared_jump,23);CHECK(value==23);}
    CHECK(shared_large[65000]==0 && shared_large[65999]==0);
    *large_import=23;shared_large[65999]=19;
    CHECK(shared_large_check()==42);
    CHECK(shared_counter==7 && shared_pointer[0]''', 1)
        source = source.replace("shared_pointer[0]='X';", "shared_pointer[0]='X';shared_large[65999]=61;")
        source = source.replace("CHECK(shared_counter==7 && shared_pointer[0]=='b');", "CHECK(shared_counter==7 && shared_pointer[0]=='b' && shared_large[65999]==19);")
        source = source.replace('puts("SHARED_LIBRARIES_OK");', '''
    {char *args[]={"bad-got",0};
     CHECK(execv("/bin/bad-got",args)==-1 && errno==ENOEXEC && shared_counter==25);
     CHECK(execv("/bin/truncated",args)==-1 && errno==ENOEXEC && shared_counter==25);}
    puts("SHARED_LIBRARIES_OK");''')
        application = directory / 'application.c'
        application.write_text(source)
        library = directory / 'libprobe.rsl.1'
        compiler = ROOT / 'ports/fuzix/tools/shared.py'
        run('python3', compiler, '--cc', library_source, '--soname', library.name, '--build', build, '-o', library)
        program = directory / 'probe.rxe'
        run('python3', compiler, '--cc', application, '--build', build,
            '--library', f'{library}=/lib/riplix/{library.name}', '-o', program)
        metadata = fmt.read(program)
        package = program.read_bytes()
        dependency = next(i for i, dep in enumerate(metadata['dependencies']) if dep['name'] == library.name)
        paths = {}
        for name in ('bad-dependency', 'bad-interface', 'bad-got', 'truncated'):
            data = bytearray(package)
            start = fmt.HEADER + dependency * fmt.DEPENDENCY
            if name == 'bad-dependency':
                data[start + 28:start + 124] = fmt.text('/lib/riplix/absent.rsl.1', 96)
            elif name == 'bad-interface':
                data[start + 124] ^= 1
            elif name == 'bad-got':
                assert metadata['got']
                got_start = fmt.HEADER + len(metadata['dependencies']) * fmt.DEPENDENCY + len(metadata['exports']) * fmt.EXPORT + len(metadata['imports']) * fmt.IMPORT
                struct.pack_into('<i', data, got_start + 8, 0x7fffffff)
            else:
                del data[-1:]
            paths[name] = directory / name
            paths[name].write_bytes(data)
        # Mismatched declarations must fail at link time rather than produce a
        # seemingly valid binary that crashes on its first cross-module call.
        mismatch = directory / 'mismatch.c'
        mismatch.write_text('extern long shared_counter;int main(void){return shared_counter;}')
        rejected = subprocess.run([sys.executable, str(compiler), '--cc', str(mismatch), '--build', str(build),
                                   '--library', f'{library}=/lib/riplix/{library.name}', '-o', str(directory/'mismatch.rxe')],
                                  cwd=ROOT, env=env, capture_output=True, text=True)
        assert rejected.returncode and 'incompatible shared declaration: shared_counter' in rejected.stderr, rejected.stderr
        paths['shared-probe'] = program
        paths['rfl-demo'] = build / 'rfl-demo.rxe'
        files = [dict(name='/lib/riplix/' + library.name, mode=0o100644, path=str(library)),
                 dict(name='/sample', text='shared loader benchmark\n')]
        if args.benchmark:
            info = json.loads(subprocess.check_output(['make', '-s', '--no-print-directory', '-C', str(ROOT/'ports/fuzix'),
                f'BUILD={build}', 'rfl-sdk-info'], cwd=ROOT, text=True))
            static = directory / 'cat-static.rxe'
            # Use the very same application object, changing only its linkage.
            run(ROOT/'src/ripple-asm/target/release/rlink', info['entry'], build/'apps/cat.pobj', *info['libraries'],
                '-f', 'rxe', '--bank-size', '64000', '-o', static, stdout=subprocess.DEVNULL)
            paths['static-cat'] = static
            paths['dynamic-cat'] = build/'cat.rxe'
        peer = None
        if args.tls:
            from fixtures import make
            from peer import Services, BINARY
            fixtures = make(directory/'fixtures')
            peer = Services(directory/'nic', fixtures)
            expected = directory/'expected'
            expected.write_bytes(BINARY)
            files += [dict(name='/etc/ssl/cert.pem', path=str(fixtures/'ca.pem')),
                      dict(name='/expected', path=str(expected))]
            paths.update(htget=build/'htget.rxe', cmp=build/'cmp.rxe')
        guest = Guest(directory, 'ripple', paths, files, peer, build_override=build, blocks=65535)
        try:
            output = guest.shell('shared-probe; echo PROBE_STATUS:$?', 180)
            print(output, flush=True)
            assert 'SHARED_LIBRARIES_OK' in output and 'PROBE_STATUS:0' in output and 'SHARED_FAILED' not in output, output
            assert output.count('SHARED_EXEC_FRESH_OK') == 2, output
            output = guest.shell('rfl-demo; echo RFL_STATUS:$?')
            print(output, flush=True)
            assert 'RFL_STATUS:0' in output, output
            if args.benchmark:
                results = {name: [] for name in ('static-cat', 'dynamic-cat')}
                for index in range(9):
                    for name in (list(results) if index % 2 == 0 else list(reversed(results))):
                        started = time.monotonic()
                        output = guest.shell(f'{name} /sample; echo BENCH_STATUS:$?')
                        results[name].append(time.monotonic()-started)
                        assert 'BENCH_STATUS:0' in output and 'shared loader benchmark' in output, output
                report = {name: dict(first=samples[0], warm_median=statistics.median(samples[1:]), samples=samples)
                          for name, samples in results.items()}
                report['sizes'] = dict(static_cat=static.stat().st_size, shared_cat=(build/'cat.rxe').stat().st_size)
                (build/'startup-benchmark.json').write_text(json.dumps(report, indent=2)+'\n')
                print(json.dumps(report, indent=2), flush=True)
            if args.tls:
                for url in ('https://tls.test/binary', 'https://tls.test:444/binary'):
                    output = guest.shell(f'htget {url} /download; echo TLS_STATUS:$?', 300)
                    print(output, flush=True)
                    assert 'TLS_STATUS:0' in output, (output, peer.failures)
                    assert 'BODY_STATUS:0' in guest.shell('cmp /expected /download; echo BODY_STATUS:$?')
                output = guest.shell('htget https://wrong.test/binary /download; echo BAD_TLS:$?', 300)
                print(output, flush=True)
                assert 'BAD_TLS:1' in output and 'TLS error -322' in output, output
                assert {'TLSv1.2', 'TLSv1.3'} <= set(peer.versions), peer.versions
        finally:
            guest.close()
            if peer:
                peer.close()
        # Bound the architectural instruction pool to fourteen user banks.
        # Distinct inodes must not alias merely because their headers claim the
        # same content identity. Cycling through these images forces eviction;
        # libc remains pinned by the running shared shell throughout.
        pressure = directory/'pressure'
        pressure.mkdir()
        copies = {}
        for index in range(20):
            path = pressure/f'cat-{index}.rxe'
            path.write_bytes((build/'cat.rxe').read_bytes())
            copies['cat-'+str(index)] = path
        guest = Guest(pressure, 'ripple', copies, [dict(name='/sample', text='CACHE_PRESSURE_OK\n')],
                      build_override=build, code_memory=46*64000)
        try:
            for index in [*range(20),0,19,0]:
                output = guest.shell(f'cat-{index} /sample; echo CACHE_STATUS:$?')
                assert 'CACHE_PRESSURE_OK' in output and 'CACHE_STATUS:0' in output, output
            print('RXE2: bounded code-cache eviction and reload passed', flush=True)
        finally:
            guest.close()
    print('RXE2: private globals, cross-bank data, callbacks, pointer identity, libc ownership, fork/COW, fresh exec, ABI checks and failed-exec rollback passed')


if __name__ == '__main__':
    main()
