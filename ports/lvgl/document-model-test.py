#!/usr/bin/env python3
"""Check Graphite document lifecycle without a GUI, kernel or personal image."""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=ROOT / 'build/lvgl')
args = parser.parse_args()
folder = args.build.resolve() / 'document-model'
folder.mkdir(parents=True, exist_ok=True)
sources = [PORT/'document-test.c', PORT/'ui-document.c', ROOT/'libraries/text/text.c', ROOT/'libraries/unicode/unicode.c']
flags = ['-DGRAPHITE_DOCUMENT_TEST_TEMP=1','-Dmkstemp=graphite_document_mkstemp','-D_POSIX_C_SOURCE=200809L','-std=c99','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all']
if sys.platform == 'darwin':
    sdk = os.environ.get('SDKROOT') or subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
    sdk_compiler = Path(sdk).parent.parent / 'usr/bin/clang'
    cc = os.environ.get('CC') or (str(sdk_compiler) if sdk_compiler.is_file() else
                                subprocess.check_output(['xcrun','--find','clang'],text=True).strip())
    flags += ['-isysroot',sdk]
else:
    cc = os.environ.get('CC','cc')
program = folder/'document-test'
subprocess.run([cc,*flags,*map(str,sources),'-o',str(program)],check=True)
with tempfile.TemporaryDirectory(prefix='graphite-documents-') as temporary:
    result = subprocess.run([str(program),temporary],check=True,capture_output=True,text=True,timeout=60)
print(result.stdout,end='')
inputs = [*sources,PORT/'ui-document.h',ROOT/'libraries/text/text.h',Path(__file__).resolve()]
report = dict(status='passed',sanitizer='undefined',private_temporary_files=True,emulated_guest_name_limit=30,
              output=result.stdout,input_sha256={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs})
(folder/'result.json').write_text(json.dumps(report,indent=2)+'\n')
