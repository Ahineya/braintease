#!/usr/bin/env python3
"""Reproduce the LVGL investigation without opening or installing OS images."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
VERSION = '9.6.0'
SHA256 = 'b20ee3acc1bba13c62d854f9ebd62e4c51e0b443b1e0225892e86442defa84df'
URL = 'https://codeload.github.com/lvgl/lvgl/tar.gz/refs/tags/v' + VERSION
KEEP = {'LV_USE_DRAW_SW', 'LV_USE_THEME_SIMPLE', 'LV_USE_FLEX', 'LV_USE_GRID',
        'LV_USE_LABEL', 'LV_USE_BUTTON', 'LV_USE_BUTTONMATRIX', 'LV_USE_CHART',
        'LV_USE_SCALE', 'LV_USE_LINE', 'LV_USE_IMAGE', 'LV_USE_TABLE', 'LV_USE_TEXTAREA'}


def capture(command, log, timeout=180):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True,
                            text=True, timeout=timeout)
    log.write_text(result.stdout + result.stderr)
    return result


def prepare(build, fetch):
    archive = build / ('lvgl-v' + VERSION + '.tar.gz')
    if not archive.exists():
        if not fetch:
            raise SystemExit('Source archive missing; run with --fetch to download the pinned release.')
        temporary = archive.with_suffix('.download')
        with urllib.request.urlopen(URL, timeout=60) as response, temporary.open('wb') as out:
            while True:
                data = response.read(1024 * 1024)
                if not data: break
                out.write(data)
        temporary.replace(archive)
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == SHA256, 'LVGL archive checksum mismatch'
    prefix = 'lvgl-' + VERSION
    source = build / prefix
    with tarfile.open(archive) as bundle:
        names = [member for member in bundle.getmembers()
                 if member.name.startswith((prefix + '/src/', prefix + '/include/'))
                 or member.name in [prefix + '/' + name for name in
                                    ('lvgl.h', 'lv_conf_template.h', 'LICENCE.txt', 'README.md')]]
        bundle.extractall(build, members=names, filter='data')
    config = build / 'config'
    config.mkdir(exist_ok=True)
    text = (source / 'lv_conf_template.h').read_text()
    text = text.replace('#if 0 /* Set this to "1" to enable content */', '#if 1 /* Investigation profile */')
    assert '#if 1 /* Investigation profile */' in text
    text = re.sub(r'(#define\s+)(LV_USE_\w+)(\s+)1\b',
                  lambda m: m[1] + m[2] + m[3] + str(int(m[2] in KEEP)), text)
    for name in ('MALLOC', 'STRING', 'SPRINTF'):
        text = re.sub(r'(#define\s+LV_USE_STDLIB_' + name + r'\s+)LV_STDLIB_BUILTIN',
                      r'\g<1>LV_STDLIB_CLIB', text)
    text = re.sub(r'(#define\s+LV_DRAW_SW_SUPPORT_(?!RGB565\s|A8\s)\w+\s+)1\b', r'\g<1>0', text)
    # A single RGB565 renderer, no OS threads, filesystem drivers, codecs, or GPU.
    text = text.replace('#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB888',
                        '#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565')
    (config / 'lv_conf.h').write_text(text)
    overlay = build / 'c16-include'
    overlay.mkdir(exist_ok=True)
    # Investigation-only printf declarations. No false claim of an 8-bit C type.
    (overlay / 'inttypes.h').write_text('#include <stdint.h>\n' + ''.join(
        '#define PRI' + kind + str(bits) + ' "' + suffix + kind + '"\n'
        for bits, suffix in ((32, 'l'), (64, 'll')) for kind in ('d', 'u', 'x', 'X')))
    old_overlay = config / 'inttypes.h'
    if old_overlay.exists(): old_overlay.unlink()
    (build / 'lvgl-source.json').write_text(json.dumps(dict(
        version='v' + VERSION, url=URL, sha256=SHA256,
        archive_octets=archive.stat().st_size), indent=2) + '\n')
    return source, config, overlay


def host(build, source, config, jobs):
    folder = build / 'host'
    folder.mkdir(exist_ok=True)
    compiler = '/usr/bin/clang' if sys.platform == 'darwin' else 'cc'
    sdk = ['-isysroot', subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()] if sys.platform == 'darwin' else []
    flags = [*sdk, '-std=c99', '-O1', '-g', '-fsanitize=undefined', '-fno-sanitize-recover=all',
             '-DLV_CONF_INCLUDE_SIMPLE=1', '-I', source, '-I', config]
    host_abi(build, compiler, flags)
    widgets = {'button', 'buttonmatrix', 'chart', 'image', 'label', 'line', 'scale', 'table', 'textarea'}
    units = []
    for unit in sorted((source / 'src').rglob('*.c')):
        parts = unit.relative_to(source / 'src').parts
        if parts[0] in ('libs', 'drivers', 'others'): continue
        if parts[0] == 'draw' and len(parts) > 2 and parts[1] not in ('sw', 'convert'): continue
        if parts[0] == 'widgets' and parts[1] not in widgets: continue
        units.append(unit)

    def compile_one(unit):
        relative = str(unit.relative_to(source)).replace('/', '_')
        output = folder / (relative + '.o')
        result = capture([compiler, *flags, '-c', unit, '-o', output], folder / (relative + '.log'))
        assert result.returncode == 0, str(unit) + '\n' + result.stderr[-4000:]
        return output

    print('Compiling', len(units), 'LVGL host reference units', flush=True)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        objects = list(pool.map(compile_one, units))
    executable = folder / 'worksheet-reference'
    result = capture([compiler, *flags, PORT / 'host_probe.c', *objects, '-lm', '-o', executable],
                     folder / 'link.log')
    assert result.returncode == 0, result.stderr[-5000:]
    result = capture([executable, folder / 'worksheet.ppm'], folder / 'run.log')
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout, end='', flush=True)
    report = dict(status='passed', kind='byte-addressed host reference only',
                  source_units=len(units), output=result.stdout.strip(),
                  executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest())
    (build / 'host-result.json').write_text(json.dumps(report, indent=2) + '\n')


def host_abi(build, compiler, flags):
    """The same representation test must succeed on ordinary byte addressing."""
    folder = build / 'host'
    folder.mkdir(exist_ok=True)
    executable = folder / 'representation'
    result = capture([compiler, *flags, PORT / 'representation.c', '-o', executable],
                     folder / 'representation-build.log')
    assert result.returncode == 0, result.stderr
    result = capture([executable], folder / 'representation-run.log')
    assert result.returncode == 0, result.stdout + result.stderr
    fields = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', result.stdout)}
    assert fields['char_bits'] == 8 and fields['second_pixel'] == 1 and fields['next_row'] == 1
    (build / 'host-abi-result.json').write_text(json.dumps(dict(status='passed',
        kind='same representation test on byte-addressed host', fields=fields,
        executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest()), indent=2) + '\n')


def c16(build, source, config, overlay, jobs):
    folder = build / 'c16'
    folder.mkdir(exist_ok=True)
    compiler = ROOT / 'target/release/rcc'
    includes = [ROOT / 'ports/fuzix/ripple/include', ROOT / 'ports/fuzix/upstream/Library/include',
                ROOT / 'runtime/include', source, config, overlay]
    flags = ['--target', 'native', '--bank-size', '64000', '-DRIPPLE_MMU=1',
             '-DFUSE_X_USER=1', '-DLV_CONF_INCLUDE_SIMPLE=1']
    flags += [x for path in includes for x in ('-I', path)]
    names = ['lv_init.c', 'core/lv_obj.c', 'display/lv_display.c', 'draw/lv_draw_buf.c',
             'draw/sw/blend/lv_draw_sw_blend_to_rgb565.c', 'font/fmt_txt/lv_font_fmt_txt.c',
             'widgets/chart/lv_chart.c', 'widgets/table/lv_table.c',
             'widgets/textarea/lv_textarea.c', 'layouts/flex/lv_flex.c']

    def compile_one(name):
        unit = source / 'src' / name
        log = folder / (name.replace('/', '_') + '.log')
        result = capture([compiler, 'compile', unit, *flags, '--emit-ir'], log)
        # Discard successful IR: these are compiler probes, not a linked port.
        log.write_text(result.stderr)
        errors = [line.strip() for line in result.stderr.splitlines()
                  if any(word in line.lower() for word in ('error', 'panicked', 'unsupported'))]
        return dict(source=name, returncode=result.returncode, diagnostics=errors[-8:], log=str(log))

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(compile_one, names))
    representation = build / 'representation.fxe'
    command = [sys.executable, ROOT / 'fuse-x/cc.py', PORT / 'representation.c',
               '-I', source, '-I', config, '-I', overlay, '-D', 'LV_CONF_INCLUDE_SIMPLE=1',
               '--static', '--float-format', '-o', representation]
    result = capture(command, build / 'representation-build.log')
    assert result.returncode == 0, result.stderr[-4000:]
    report = dict(kind='C16 compiler probes only', compiler_sha256=hashlib.sha256(compiler.read_bytes()).hexdigest(),
                  passed=sum(r['returncode'] == 0 for r in results), total=len(results), units=results)
    (build / 'c16-result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('C16 source probes:', report['passed'], '/', report['total'], flush=True)


def guest(build):
    sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
    from guest import Guest
    folder = build / 'guest'
    folder.mkdir(exist_ok=True)
    vm = Guest(folder, 'native', {'lvgl-abi': build / 'representation.fxe'})
    try:
        output = vm.shell('lvgl-abi; echo ABI_STATUS:$?')
        assert 'ABI_STATUS:0' in output and 'LVGL_ABI' in output, output
        match = re.search(r'LVGL_ABI ([^\r\n]+)', output)
        fields = dict((key, int(value)) for key, value in re.findall(r'(\w+)=(\d+)', match[1]))
        assert fields['char_bits'] == 16
        assert fields['second_pixel'] == 0 and fields['next_row'] == 0
        (build / 'guest-result.json').write_text(json.dumps(dict(status='passed',
            kind='confirms unadapted RGB565 pointer arithmetic is incompatible', fields=fields,
            executable_sha256=hashlib.sha256((build / 'representation.fxe').read_bytes()).hexdigest()), indent=2) + '\n')
        print(output, end='', flush=True)
    finally:
        vm.close()
    (folder / 'disk.img').unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['prepare', 'host', 'c16', 'guest', 'all'])
    parser.add_argument('--build', type=Path, default=ROOT / 'build/gui-evaluation')
    parser.add_argument('--fetch', action='store_true', help='download the checksum-pinned official source archive')
    parser.add_argument('-j', '--jobs', type=int, default=4)
    args = parser.parse_args()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    source, config, overlay = prepare(build, args.fetch)
    if args.mode in ('host', 'all'): host(build, source, config, args.jobs)
    if args.mode in ('c16', 'all'): c16(build, source, config, overlay, args.jobs)
    if args.mode in ('guest', 'all'): guest(build)


if __name__ == '__main__':
    main()
