#!/usr/bin/env python3
"""Compare identical-object RXE1/RXE2 packages, counting shared libraries once."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from shared.format import read

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT/'build/riplix-shared')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    build = args.build.resolve()
    # Ask Make for the actual static SDK selection rather than globbing stale
    # objects from an incremental directory.
    groups = dict(cat='$(BUILD)/apps/cat.pobj',
                  sh='$(SHELL_OBJS) $(BUILD)/sh/input.pobj $(BUILD)/sh/jobs.pobj $(BUILD)/sh/prompt.pobj $(UNICODE_OBJS)',
                  mux='$(BUILD)/apps/mux.pobj $(UNICODE_OBJS)',
                  cursescheck='$(BUILD)/user/cursescheck.pobj $(CONFIG_OBJS) $(CURSES_OBJS)',
                  htget='$(BUILD)/tls/ripple/htget.pobj',
                  chat=' '.join('$(BUILD)/chat/'+name+'.pobj' for name in
                      ('main','ui','composer','mentions','json','store','http','network','tools','tool_history'))+' $(CURSES_OBJS)')
    query = 'shared-measure-inputs:\n' + ''.join(
        f'\t@echo {name} $(BUILD)/user/entry.pobj {objects} $(BUILD)/user/userlib.pobj $(LIBC_OBJS)\n'
        for name, objects in groups.items())
    result = subprocess.check_output(['make', '-s', '--no-print-directory', '-C', str(ROOT/'ports/fuzix'),
        f'BUILD={build}', '-f', 'Makefile', '-f', '-', 'shared-measure-inputs'], input=query, text=True)
    inputs = {fields[0]: fields[1:] for fields in map(str.split, result.splitlines())}
    manifest = build/'tls/ripple/objects.json'
    if manifest.exists():
        tls = [str(Path(p).with_suffix('.pobj')) for p in json.loads(manifest.read_text())]
        for name in ('htget','chat'):inputs[name][2:2] = tls
    packages, libraries = {}, {}
    def dependencies(path):
        for dep in read(path)['dependencies']:
            if dep['path'] not in libraries:
                library = build/'lib'/Path(dep['path']).name
                libraries[dep['path']] = library.stat().st_size
                dependencies(library)
    with tempfile.TemporaryDirectory(prefix='riplix-measure-') as temporary:
        for name, objects in inputs.items():
            shared = build/(name+'.rxe')
            if not shared.exists() or not all(Path(p).exists() for p in objects):
                continue
            assert shared.read_bytes()[:4] == b'RXE2', f'{shared} is not a shared build'
            static = Path(temporary)/(name+'.rxe')
            subprocess.run([str(ROOT/'src/ripple-asm/target/release/rlink'), *objects, '-f', 'rxe',
                '--bank-size', '64000', '-o', str(static)], check=True, stdout=subprocess.DEVNULL)
            before, after = static.stat().st_size, shared.stat().st_size
            packages[name] = dict(static=before, shared=after, saved_percent=round(100*(1-after/before),2))
            dependencies(shared)
    before = sum(p['static'] for p in packages.values())
    after = sum(p['shared'] for p in packages.values())+sum(libraries.values())
    report = dict(unit='host octets', applications=packages, libraries=libraries,
                  total=dict(static=before, shared_with_libraries=after, saved_percent=round(100*(1-after/before),2)))
    output = args.output or build/'shared-sizes.json'
    output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
