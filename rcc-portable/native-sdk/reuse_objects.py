"""Explicit, hash-checked reuse of objects recovered from a stopped build.

The recovery record must come from independently checking the original guest
disk, its installed input files and completed objects. This is an incremental
build, never a fresh compiler generation or an accepted checkpoint by itself.
"""
import json
from pathlib import Path

from test_make_guest import digest
from test_x86 import file


def prepare_reuse(path, source, rows, files, results):
    path = Path(path)
    record = json.loads(path.read_text())
    inputs = record['inputs']
    current = {f['name']: f for f in files}
    if (record.get('status') != 'verified-recovered-inputs'
            or not record.get('original_disk_sha256') or not record.get('previous_key')
            or set(inputs) != set(current)):
        raise ValueError('recovered input set/provenance does not match this build')
    # These two resource fixes preserve object semantics. All source, SDK,
    # frontend, QBE, assembler, driver and other inputs must be identical.
    changed = {}
    for name, old in inputs.items():
        actual = digest(Path(current[name]['path']))
        if actual != old:
            if name not in ['/bin/rcc-qbe', '/bin/rcc-x64-ld']:
                raise ValueError('changed recovered-build input: ' + name)
            changed[name] = dict(previous=old, current=actual)
    expected = {Path(r['object']).name: r['object'] for r in rows}
    objects = record['objects']
    if not objects or not set(objects).issubset(expected):
        raise ValueError('invalid recovered object set')
    for name, checksum in objects.items():
        if Path(name).name != name or digest(path.parent / 'partial-objects' / name) != checksum:
            raise ValueError('changed recovered object: ' + name)
    # Make must not rebuild the verified objects merely because the replacement
    # seed has a newer timestamp. Remove only those rules; remaining source and
    # header dependencies and the final link rule stay in the original order.
    lines = (source / 'Makefile').read_text().splitlines(True)
    targets = {expected[n] for n in objects}
    output, removed, skip = [], set(), False
    for line in lines:
        if line and not line[0].isspace():
            target = line.split(':', 1)[0]
            skip = target in targets
            if skip:
                removed.add(target)
                output.append(target + ':\n')
        if not skip:
            output.append(line)
    if removed != targets:
        raise ValueError('missing recovered object Make rules')
    make = results / 'resume.Makefile'
    make.write_text(''.join(output))
    files = [file(f['name'], make) if f['name'] == '/tmp/native-tools/Makefile'
             else f for f in files]
    files += [file('/tmp/native-tools/' + expected[name], path.parent / 'partial-objects' / name)
              for name in objects]
    return files, dict(previous_key=record['previous_key'],
        original_disk_sha256=record['original_disk_sha256'], objects=objects,
        input_record_sha256=digest(path), replaced_seeds=changed,
        original_make_sha256=inputs['/tmp/native-tools/Makefile'],
        resume_make_sha256=digest(make), newly_compiled_units=len(rows) - len(objects))
