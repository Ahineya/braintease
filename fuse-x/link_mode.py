#!/usr/bin/env python3
"""Maintain a timestamp prerequisite for Make's application link mode."""
import argparse
import os
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    stamp = args.build.resolve() / '.link-mode'
    content = 'static\n' if os.environ.get('FUSE_X_SHARED_LIBRARIES', '1') == '0' else 'shared\n'
    stamp.parent.mkdir(parents=True, exist_ok=True)
    if not stamp.exists() or stamp.read_text() != content:
        stamp.write_text(content)
    print(stamp)


if __name__ == '__main__':
    main()
