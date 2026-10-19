"""Embed the optional HTTP module without depending on guest installation paths."""
import json
from pathlib import Path


def prepare_http(destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    source = (Path(__file__).parent / 'http.mjs').read_text()
    output = '/* Generated from ports/quickjs/http.mjs. */\nstatic const char qjs_http_source[] =\n'
    output += '\n'.join(json.dumps(line, ensure_ascii=True) for line in source.splitlines(keepends=True)) + ';\n'
    path = destination / 'qjs-http.inc'
    if not path.exists() or path.read_text() != output:
        path.write_text(output)
    return path
