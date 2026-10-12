#!/usr/bin/env python3
"""Extract the actual wall-column loop for an isolated native/Ripple fixture.

Usage: extract_projection_fixture.py prepared/r_segs.c generated
This avoids linking the rest of the renderer just to exercise clipping widths.
"""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
source = args.source.read_text()
preamble = source[:source.index('//\n// R_RenderMaskedSegRange')]
start = source.index('#define HEIGHTBITS')
end = source.index('//\n// R_StoreWallRange', start)
args.output.mkdir(parents=True, exist_ok=True)
(args.output / 'projection_loop.inc').write_text(preamble + source[start:end])
