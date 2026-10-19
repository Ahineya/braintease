"""Content-based build stamps, shared by the launchers and guest compiler package.

Timestamps alone are insufficient for generated sources. Inputs include their
paths and contents; outputs are checked too, so deleting an artifact repairs it.
Only successful builds publish a stamp. Callers serialize builds as necessary.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess

EXCLUDED = {'build', 'target', '.git', '__pycache__', 'node_modules'}


def sources(*roots):
    repository = Path(__file__).resolve().parent.parent
    resolved = [Path(p).resolve() for p in roots]
    if all(p.is_relative_to(repository) for p in resolved):
        # Respect each subtree's ignore rules: generated runtime assembly must
        # not become a source dependency of the build which creates it.
        result = subprocess.run(['git', '-C', str(repository), 'ls-files', '-z',
                                 '--cached', '--others', '--exclude-standard', '--',
                                 *[str(p.relative_to(repository)) for p in resolved]],
                                capture_output=True, check=True)
        return sorted({repository / os.fsdecode(p) for p in result.stdout.split(b'\0') if p})
    result = set()
    for root in map(Path, roots):
        if root.is_file():
            result.add(root.resolve())
        elif root.is_dir():
            for directory, dirs, files in os.walk(root):
                dirs[:] = sorted(d for d in dirs if d not in EXCLUDED and not d.startswith('.'))
                for name in files:
                    path = Path(directory) / name
                    if path.suffix not in {'.pyc', '.log', '.pobj', '.par', '.fxe', '.rxe', '.bf', '.bfm'}:
                        result.add(path.resolve())
        else:
            result.add(root.resolve())  # Missing prerequisites change the key too.
    return sorted(result)


def fingerprint(paths, settings=()):
    digest = hashlib.sha256(json.dumps(settings, sort_keys=True, default=os.fspath).encode())
    for path in sorted(set(map(Path, paths))):
        digest.update(str(path.resolve()).encode() + b'\0')
        if not path.is_file():
            digest.update(b'missing\0')
            continue
        with path.open('rb') as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(chunk)
        digest.update(b'\0')
    return digest.hexdigest()


def outputs_match(record):
    outputs = record.get('outputs', {})
    return bool(outputs) and all(Path(p).is_file() and fingerprint([p]) == key
                                 for p, key in outputs.items())


def cached_step(stamp, inputs, settings, build, outputs, force=False):
    stamp = Path(stamp)
    key = fingerprint(inputs, settings)
    try:
        previous = json.loads(stamp.read_text())
    except (OSError, ValueError):
        previous = {}
    if not force and previous.get('key') == key and outputs_match(previous):
        return False
    # A failed forced rebuild must not leave an older success usable.
    stamp.unlink(missing_ok=True)
    build()
    paths = list(outputs() if callable(outputs) else outputs)
    if not paths or any(not Path(p).is_file() for p in paths):
        raise RuntimeError('build succeeded without its expected outputs')
    record = {'key': key, 'outputs': {str(Path(p).resolve()): fingerprint([p]) for p in paths}}
    stamp.parent.mkdir(parents=True, exist_ok=True)
    temporary = stamp.with_name(stamp.name + f'.tmp-{os.getpid()}')
    try:
        temporary.write_text(json.dumps(record, indent=2) + '\n')
        os.replace(temporary, stamp)
    finally:
        temporary.unlink(missing_ok=True)
    return True


def c_dependencies(source, include_dirs):
    """Conservatively follow literal includes, including .c/.inc fragments.

    All conditional arms are scanned. Macro-selected headers must additionally
    be represented by the caller's configuration/header fingerprint.
    """
    import re
    pending, seen = [Path(source)], set()
    while pending:
        path = pending.pop().resolve()
        if path in seen or not path.is_file():continue
        seen.add(path)
        text = path.read_text(errors='replace')
        for opening, name in re.findall(r'^\s*#\s*include\s*(["<])([^">]+)[">]', text, re.M):
            directories = ([path.parent] if opening == '"' else []) + list(map(Path, include_dirs))
            for directory in directories:
                candidate = directory / name
                if candidate.is_file():
                    pending.append(candidate);break
    return sorted(seen)
