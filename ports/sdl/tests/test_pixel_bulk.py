#!/usr/bin/env python3
"""Run public SDL bulk-operation guards on upstream and actual C16 SDL."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
from guest import Guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'build/sdl')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/sdl/pixel-bulk-tests')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    expected = subprocess.check_output([str((args.build/'sdl-bulk-host').resolve())], text=True)
    assert re.fullmatch(r'SDL_BULK cases=\d+ passed\n', expected), expected
    (args.output/'host.log').write_text(expected)
    with tempfile.TemporaryDirectory(prefix='sdl-pixel-bulk-') as directory:
        guest = Guest(directory, 'native', {'sdl-bulk': (args.build/'sdl-bulk.fxe').resolve()}, blocks=32768)
        try:
            output = guest.shell('sdl-bulk; echo SDL_BULK_STATUS:$?', seconds=120)
            assert expected.strip() in output and 'SDL_BULK_STATUS:0' in output, output
        finally:
            try:
                guest.close()
            finally:
                (args.output/'guest.log').write_bytes(guest.log)
    (args.output/'result.json').write_text(json.dumps(dict(status='passed', summary=expected.strip()), indent=2)+'\n')
    print(expected.strip()+' on host and Fuse-X')


if __name__ == '__main__':
    main()
