#!/usr/bin/env python3
"""Exercise the portable text engine, independent of curses or either kernel."""
import argparse
from pathlib import Path
import shlex
import re
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
# Binary search requires sorted, unique tables and a bounded token buffer.
syntax = (ROOT / 'libraries/text/syntax.inc').read_text()
for name in ('core', 'pascal', 'script', 'lisp', 'json', 'basic', 'javascript', 'syma'):
    body = re.search(r'const char \*const ' + name + r'\[\] = \{(.*?)\};', syntax, re.S).group(1)
    words = re.findall(r'"([^"]+)"', body)
    assert words == sorted(set(words)) and all(len(word) < 32 for word in words), name
js_syntax = (ROOT / 'libraries/text/javascript.inc').read_text()
words = re.findall(r'"([^"\n]+)"', re.search(r'const char \*const expressions\[\] = \{(.*?)\};', js_syntax, re.S).group(1))
assert words == sorted(set(words)), 'JavaScript operand contexts'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc', help='compiler command, including optional SDK flags')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='ripple-text-') as temporary:
    for name in ('text', 'text_unicode'):
        executable = Path(temporary) / (name + '-test')
        subprocess.run([*shlex.split(args.cc), '-std=c99', '-Wall', '-Wextra', '-Werror',
                        str(ROOT / 'libraries/tests' / (name + '.c')), str(ROOT / 'libraries/text/text.c'),
                        str(ROOT / 'libraries/unicode/unicode.c'), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
