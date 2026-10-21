#!/usr/bin/env python3
"""Validate native scalar lowering and record changes for incremental builds."""
import os
from pathlib import Path


def selected():
    value = os.environ.get('RCC_NATIVE_MEMORY', 'checked')
    if value not in ('checked', 'read-v1'):
        raise ValueError('RCC_NATIVE_MEMORY must be checked or read-v1')
    return value


if __name__ == '__main__':
    import argparse
    import tempfile
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    try:
        value = selected() + '\n'
    except ValueError as error:
        parser.error(str(error))
    args.build.mkdir(parents=True, exist_ok=True)
    stamp = args.build / 'native-memory-mode'
    if not stamp.exists() or stamp.read_text() != value:
        with tempfile.NamedTemporaryFile(mode='w', dir=args.build, delete=False) as file:
            file.write(value)
            temporary = file.name
        os.replace(temporary, stamp)
    print(stamp)
