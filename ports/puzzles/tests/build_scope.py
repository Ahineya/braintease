#!/usr/bin/env python3
"""Exercise real build dispatch without compiling or touching OS artifacts."""
from contextlib import ExitStack
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'fuse-x'))
spec = importlib.util.spec_from_file_location('graphics_build', ROOT / 'fuse-x/graphics.py')
graphics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(graphics)
names = {g['id'] for g in json.loads((ROOT / 'ports/puzzles/catalog.json').read_text())}


class NoCompilation:
    def __init__(self, **kwargs): pass
    def __enter__(self): return self
    def __exit__(self, *args): pass
    def map(self, *args): return []


def dispatch(only):
    with tempfile.TemporaryDirectory(prefix='puzzles-dispatch-') as temporary, ExitStack() as stack:
        build = Path(temporary)
        (build / 'nanox-native').mkdir()
        generate = Mock(return_value=build / 'puzzles/src')
        adapter = SimpleNamespace(generate=generate, COMMON=[], EXTRA={n: [] for n in names})
        commands = []
        stack.enter_context(patch.dict(sys.modules, {'adapt': adapter}))
        stack.enter_context(patch.object(sys, 'argv', ['graphics.py', '--build', temporary, *only]))
        stack.enter_context(patch.object(graphics, 'ThreadPoolExecutor', NoCompilation))
        stack.enter_context(patch.object(graphics, 'c_dependencies', return_value=[]))
        stack.enter_context(patch.object(Path, 'read_bytes', return_value=b''))
        stack.enter_context(patch.object(Path, 'rglob', return_value=[]))
        stack.enter_context(patch.object(graphics.subprocess, 'run', side_effect=lambda command, **kw: commands.append(command)))
        graphics.main()
        linked = {Path(c[c.index('-o') + 1]).stem for c in commands if '-o' in c}
        return linked, generate.call_count


def main():
    linked, generated = dispatch([])
    assert {'nano-X', 'nxterm', 'nxlaunch'} <= linked
    assert not linked & (names | {'puzzles'})
    assert generated == 0
    assert dispatch(['--only', 'nxterm']) == ({'nxterm'}, 0)
    assert dispatch(['--only', 'net']) == ({'net'}, 1)
    assert dispatch(['--only', 'puzzles', *sorted(names)]) == (names | {'puzzles'}, 1)
    print('Build dispatch: normal graphics exclude puzzles; explicit collection builds remain available')


if __name__ == '__main__':
    main()
