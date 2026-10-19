#!/usr/bin/env python3
"""Host reference: binary/UTF-8 response and redirect policy edge cases."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--binary', required=True, type=Path)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='qjs-http-protocol-') as tmp:
    worker = Path(tmp) / 'htget'
    # Pin the running Python instead of relying on an env shebang.
    import sys
    worker.write_text('#!' + sys.executable + '\n' + (PORT / 'tests/http-worker.py').read_text().split('\n', 1)[1])
    worker.chmod(0o755)
    result = subprocess.run([str(a.binary.resolve()), str(PORT / 'tests/http-protocol.mjs')],
        env=dict(os.environ, JS_HTTP_HELPER=str(worker)), capture_output=True, timeout=60)
    output = (result.stdout + result.stderr).decode()
    assert result.returncode == 0 and 'HTTP PROTOCOL CHECKS 16\n' in output, output
    print(output, end='')
