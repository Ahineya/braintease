#!/usr/bin/env python3
"""Audit the official X.Org build files; this does not build an X server."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
VERSION = '21.1.25'
PREFIX = 'xorg-server-' + VERSION
URL = 'https://www.x.org/releases/individual/xserver/' + PREFIX + '.tar.xz'
SHA256 = '6ad4e3c7b59a309b32c92e5c15ce1267110f9e13f1ac78da62361a628da1a0eb'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/gui-evaluation')
    parser.add_argument('--fetch', action='store_true')
    args = parser.parse_args()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    archive = build / (PREFIX + '.tar.xz')
    if not archive.exists():
        if not args.fetch:
            raise SystemExit('Source archive missing; use --fetch to download the pinned X.Org release.')
        temporary = archive.with_suffix('.download')
        with urllib.request.urlopen(URL, timeout=60) as response, temporary.open('wb') as out:
            while True:
                block = response.read(1024 * 1024)
                if not block: break
                out.write(block)
        temporary.replace(archive)
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == SHA256
    files = ['meson.build', 'meson_options.txt', 'COPYING', 'configure.ac', 'include/misc.h',
             'include/os.h', 'hw/kdrive/meson.build', 'hw/kdrive/ephyr/meson.build']
    with tarfile.open(archive) as bundle:
        members = [bundle.getmember(PREFIX + '/' + name) for name in files]
        bundle.extractall(build, members=members, filter='data')
    source = build / PREFIX
    meson = (source / 'meson.build').read_text()
    kdrive = (source / 'hw/kdrive/meson.build').read_text()
    ephyr = (source / 'hw/kdrive/ephyr/meson.build').read_text()
    options = (source / 'meson_options.txt').read_text()
    dependencies = re.findall(r"dependency\('([^']+)'", meson)
    assert all(name in dependencies for name in ('pixman-1', 'xfont2', 'xtrans', 'xkbfile'))
    assert "subdir('ephyr')" in kdrive and 'fbdev' not in kdrive
    assert "dependency('xcb')" in ephyr
    assert "option('mitshm'" in options and "option('glx'" in options
    mapping = ROOT / 'ports/fuzix/upstream/Library/include/sys/mman.h'
    socket = ROOT / 'ports/fuzix/upstream/Library/include/sys/socket.h'
    report = dict(status='passed', kind='source and OS header audit only; no X11 runtime',
                  version=VERSION, url=URL, sha256=SHA256,
                  dependencies=dependencies,
                  kdrive_subdirectories=re.findall(r"subdir\('([^']+)'", kdrive),
                  xephyr_dependencies=re.findall(r"dependency\('([^']+)'", ephyr),
                  current_mman_is_empty='Nothing here for now' in mapping.read_text(),
                  socket_header_declares_sendmsg='sendmsg(' in socket.read_text(),
                  files={name: hashlib.sha256((source / name).read_bytes()).hexdigest() for name in files})
    (build / 'xserver-result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('X.Org audit passed: Kdrive is Xephyr; no ready framebuffer backend; mmap/FD transfer need platform work.')


if __name__ == '__main__':
    main()
