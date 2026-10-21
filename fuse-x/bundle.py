#!/usr/bin/env python3
"""Give hosted windows one macOS application identity without presenter apps."""
import argparse
from pathlib import Path
import plistlib
import shutil
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
args = parser.parse_args()
font_assets = Path(__file__).resolve().parents[1] / 'libraries/graphics/desktop-fonts'
licenses = args.build / 'licenses'
licenses.mkdir(parents=True, exist_ok=True)
shutil.copy2(font_assets / 'LICENSE.NotoSans.txt', licenses / 'LICENSE.NotoSans.txt')
if sys.platform == 'darwin':
    contents = args.build / 'Fuse-X.app/Contents'
    binaries = contents / 'MacOS'
    binaries.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.build / 'fuse-x', binaries / 'fuse-x')
    resources = contents / 'Resources/licenses'
    resources.mkdir(parents=True, exist_ok=True)
    shutil.copy2(font_assets / 'LICENSE.NotoSans.txt', resources / 'LICENSE.NotoSans.txt')
    (contents / 'Info.plist').write_bytes(plistlib.dumps(dict(
        CFBundleIdentifier='org.braintease.fuse-x', CFBundleName='Fuse-X',
        CFBundleDisplayName='Fuse-X', CFBundleExecutable='fuse-x',
        CFBundlePackageType='APPL', CFBundleVersion='1', CFBundleShortVersionString='0.1',
        NSHighResolutionCapable=True, NSPrincipalClass='NSApplication')))
