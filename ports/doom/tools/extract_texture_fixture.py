#!/usr/bin/env python3
"""Extract the actual texture cache routines for a native/Ripple fixture."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
source = args.source.read_text()
args.output.mkdir(parents=True, exist_ok=True)
(args.output / "texture_cache.inc").write_text(
    source[:source.index("static void GenerateTextureHashTable(void)")])
