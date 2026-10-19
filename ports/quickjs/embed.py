"""Embed optional host source modules without guest installation-path dependencies."""
import json
from pathlib import Path


def prepare_http(destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    output = '/* Generated optional QuickJS host modules. */\n'
    for name, file in [('http', 'http.mjs'), ('net', 'net.mjs'), ('server', 'server.mjs')]:
        source = (Path(__file__).parent / file).read_text()
        output += 'static const char qjs_' + name + '_source[] =\n'
        output += '\n'.join(json.dumps(line, ensure_ascii=True) for line in source.splitlines(keepends=True)) + ';\n'
    path = destination / 'qjs-http.inc'
    if not path.exists() or path.read_text() != output:
        path.write_text(output)
    return path
