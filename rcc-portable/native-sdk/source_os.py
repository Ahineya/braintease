#!/usr/bin/env python3
"""Freeze the current C16 x64 OS sources for a guest frontend build.

The native platform, IR-to-machine-code backend and final image tools remain
explicit host inputs. This stage never claims a fully self-hosted OS build.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def relative(path, root):
    return Path(path).resolve().relative_to(root.resolve()).as_posix()


def compile_command(line, compiler):
    words = shlex.split(line)
    if not words or words[0] != str(compiler) or words[1:2] != ['compile']:
        return None
    end = words.index('--emit-ir')
    source, options = words[2], words[3:end]
    if words[end + 1] != '>':
        raise ValueError('unrecognized OS compiler output redirection')
    destination = words[end + 2]
    if not destination.endswith('.ir.tmp'):
        raise ValueError('expected temporary OS IR output')
    return source, options, destination[:-4]


def portable_options(options, root):
    result, i = [], 0
    while i < len(options):
        option = options[i]
        i += 1
        if option == '--bank-size':
            if options[i] != '64000':
                raise ValueError('guest frontend requires the existing 64000-cell banks')
            i += 1
        elif option == '--target':
            if options[i] != 'native':
                raise ValueError('expected native OS IR')
            result += [option, options[i]]
            i += 1
        elif option == '-I':
            result += ['-I', 'src/' + relative(options[i], root)]
            i += 1
        elif option.startswith('-I'):
            result.append('-Isrc/' + relative(option[2:], root))
        elif option.startswith(('-D', '-U')):
            result.append(option)
        else:
            raise ValueError('unhandled OS compiler option: ' + option)
    return result


def adapt_source(name, data):
    """Strict-C declarations/casts in the copy; no shared-source ABI changes."""
    replacements = {}
    if name == 'runtime/include/string.h':
        replacements = {
            'char *src': 'const char *src',
            'char *str);': 'const char *str);',
            'char *str, int c': 'const char *str, int c',
            'char *s1, char *s2': 'const char *s1, const char *s2',
            'void *src': 'const void *src',
            'void *s1, void *s2': 'const void *s1, const void *s2',
        }
    elif name == 'ports/fuzix/ripple/namecache.inc':
        replacements = {'#define NAME_CACHE_SIZE 64':
                        'void namecpy(uint8_t *, uint8_t *, unsigned);\n#define NAME_CACHE_SIZE 64'}
    elif name == 'ports/fuzix/ripple/raster-abi.h':
        replacements = {'valaddr_w(r->dst,': 'valaddr_w((const uint8_t *)r->dst,',
                        'valaddr_r(r->src,': 'valaddr_r((const uint8_t *)r->src,'}
    elif name == 'ports/fuzix/ripple/pcm.c':
        replacements = {'unsigned live,status;': 'unsigned short live;unsigned status;',
                        'valaddr_r(input[i].data,': 'valaddr_r((const uint8_t *)input[i].data,'}
    elif name == 'ports/fuzix/ripple/crypto-device.h':
        replacements = {'valaddr_w(pointer,': 'valaddr_w((const uint8_t *)pointer,'}
    elif name == 'ports/fuzix/ripple/console.c':
        replacements = {'unsigned n, done, status, i;':
                        'unsigned short done;\n    unsigned n, status, i;'}
    elif name == 'fuse-x/graphics.c':
        replacements = {'valaddr_w(user,': 'valaddr_w((const uint8_t *)user,',
                        'valaddr_r(t.data,': 'valaddr_r((const uint8_t *)t.data,',
                        'valaddr_w(c.text,': 'valaddr_w((const uint8_t *)c.text,',
                        'valaddr_r(c.text,': 'valaddr_r((const uint8_t *)c.text,',
                        'valaddr_r(indexed.palette,': 'valaddr_r((const uint8_t *)indexed.palette,',
                        'valaddr_r(u.pixels,': 'valaddr_r((const uint8_t *)u.pixels,'}
    elif name == 'fuse-x/sysdev.c':
        replacements = {'valaddr_w(p,': 'valaddr_w((const uint8_t *)p,'}
    elif name == 'ports/fuzix/upstream/Kernel/include/netdev.h':
        # sockets.c already namespaces this unused legacy daemon hook locally.
        # The portable frontend retains declarations from other kernel units.
        replacements = {'extern void *net_poll(void);':
                        'extern void *fuzix_native_net_poll(void);'}
    if not replacements:
        return data, []
    text, changes = data.decode(), []
    for old, new in replacements.items():
        count = text.count(old)
        if not count:
            raise ValueError('OS source adapter no longer matches ' + name + ': ' + old)
        text = text.replace(old, new)
        changes.append(dict(before=old, after=new, count=count))
    return text.encode(), changes


def prepare(build, output, compiler):
    build, output, compiler = build.resolve(), output.resolve(), compiler.resolve()
    if output.exists():
        raise ValueError('use a new source output directory to preserve frozen inputs')
    kernel = (build / 'freestanding-kernel-inputs.txt').read_text().splitlines()
    applications = (build / 'freestanding-app-inputs.txt').read_text().splitlines()
    expected = {str(Path(p).resolve()): 'kernel' for p in kernel}
    expected.update({str(Path(p).resolve()): 'application' for p in applications})
    dry = subprocess.check_output(['make', '-Bn', '-C', str(ROOT / 'fuse-x'),
        'BUILD=' + str(build), 'RCC=' + str(compiler),
        'FUSE_X_HEAP_PROFILE=bounded', 'freestanding-ir'], text=True)
    rows = []
    for line in dry.splitlines():
        command = compile_command(line, compiler)
        if command is None:
            continue
        source, options, target = command
        target = str(Path(target).resolve())
        if target not in expected:
            continue
        source = Path(source)
        if not source.is_absolute():
            source = ROOT / 'fuse-x' / source
        rows.append(dict(kind=expected[target], source='src/' + relative(source, ROOT),
                         options=portable_options(options, ROOT),
                         output='ir/u' + str(len(rows)) + '.ir', original_ir=target))
    if {r['original_ir'] for r in rows} != set(expected) or len(rows) != len(expected):
        raise ValueError('OS source commands do not cover the exact linked IR inventory')
    output.mkdir(parents=True)
    # Preserve relative include relationships, including C/.inc inclusions.
    # Generated source adapters are captured too, with their actual source path.
    bases = ['fuse-x', 'ports/fuzix/upstream/Kernel', 'ports/fuzix/ripple',
             'runtime', 'tools/x86_64/riplix', 'ports/graphics']
    copied = set()
    for base in bases:
        for parent, dirs, files in os.walk(ROOT / base):
            dirs[:] = [d for d in dirs if d not in ('build', 'target', '__pycache__', '.git')]
            for name in files:
                p = Path(parent) / name
                if p.suffix in ('.c', '.h', '.inc'):
                    copied.add(p)
    copied.update(ROOT / r['source'][4:] for r in rows)
    adapters = {}
    for source in sorted(copied):
        destination = output / 'src' / source.relative_to(ROOT)
        destination.parent.mkdir(parents=True, exist_ok=True)
        original = source.read_bytes()
        data, changes = adapt_source(str(source.relative_to(ROOT)), original)
        destination.write_bytes(data)
        if changes:
            adapters[str(destination.relative_to(output))] = dict(
                original_sha256=hashlib.sha256(original).hexdigest(), changes=changes)
    files = {str(p.relative_to(output)): digest(p) for p in sorted(output.rglob('*')) if p.is_file()}
    make = ['# Frozen OS frontend build; backend and hardware objects are host inputs.',
            'CC = /bin/rcc-c', '.DELETE_ON_ERROR:', '.PHONY: all',
            'all: ' + ' '.join(r['output'] for r in rows)]
    for r in rows:
        make += [r['output'] + ': ' + r['source'], '\tmkdir -p ir',
                 '\t$(CC) ' + shlex.join([*r['options'], r['source'], '-o', r['output']])]
    (output / 'Makefile').write_text('\n'.join(make) + '\n')
    files['Makefile'] = digest(output / 'Makefile')
    manifest = dict(stage='guest-c16-os-frontend', rows=rows, files=files, adapters=adapters,
        host_dependencies=['generated source adapters', 'IR optimizer/native backend',
                           'native platform C/assembly objects', 'EFI and image tools'])
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Prepared', len(rows), 'OS source units:', output, flush=True)
    return manifest


def assess(source, compiler):
    manifest = json.loads((source / 'manifest.json').read_text())
    for name, checksum in manifest['files'].items():
        if digest(source / name) != checksum:
            raise ValueError('frozen OS source changed: ' + name)
    output = source / 'host-ir'
    output.mkdir(exist_ok=True)
    rows = []
    for r in manifest['rows']:
        destination = output / Path(r['output']).name
        command = [str(compiler.resolve()), *r['options'], r['source'], '-o', str(destination)]
        result = subprocess.run(command, cwd=source, capture_output=True, text=True)
        destination.with_suffix('.log').write_text(result.stderr)
        row = dict(source=r['source'], output=r['output'], status='failed' if result.returncode else 'passed',
                   diagnostic=result.stderr.splitlines()[:8])
        if result.returncode == 0:
            row['sha256'] = digest(destination)
        else:
            destination.unlink(missing_ok=True)
        rows.append(row)
        print(row['status'], r['source'], '\n'.join(row['diagnostic']), flush=True)
    report = dict(status='passed' if all(r['status'] == 'passed' for r in rows) else 'blocked',
                  manifest_sha256=digest(source / 'manifest.json'),
                  frontend_sha256=digest(compiler), units=rows)
    (source / 'assessment.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    p.add_argument('--assess', type=Path, help='host portable frontend; assess an existing source bundle')
    a = p.parse_args()
    if a.assess:
        if assess(a.output.resolve(), a.assess)['status'] != 'passed':
            raise SystemExit(1)
    elif a.build:
        prepare(a.build, a.output, a.compiler)
    else:
        p.error('--build or --assess is required')


if __name__ == '__main__':
    main()
