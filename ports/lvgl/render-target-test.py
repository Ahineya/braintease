#!/usr/bin/env python3
"""Exercise retained GX targets in a fresh offline guest, never a personal disk."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
OUT = ROOT / 'build/lvgl/render-target-tests'
SOURCES = (
    'ports/lvgl/render-check.c', 'ports/lvgl/render-target-test.py',
    'ports/lvgl/render-lvgl-check.c', 'ports/lvgl/render-lvgl-test.py',
    'ports/lvgl/gx.c', 'ports/lvgl/gx-port.h', 'ports/lvgl/adapt.py',
    'ports/lvgl/plot-drag-test.py',
    'ports/lvgl/plot-raster.c', 'ports/lvgl/plot-raster.h',
    'ports/graphics/include/gx.h', 'fuse-x/graphics.c', 'fuse-x/render-abi.h',
    'libraries/graphics/render.rs',
    'rcc-native-runtime/src/hosted_render.rs',
    'rcc-native-runtime/src/hosted_graphics.rs',
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'result.json').unlink(missing_ok=True)
    kernel = Path(os.environ.get('FUSE_X_TEST_KERNEL', ROOT / 'fuse-x/build/fuse-x')).resolve()
    kernel_hash = digest(kernel)
    sources = {name: digest(ROOT / name) for name in SOURCES}
    application = OUT / 'render-check.fxe'
    subprocess.run([
        sys.executable, str(ROOT / 'fuse-x/cc.py'), str(PORT / 'render-check.c'),
        str(PORT / 'plot-raster.c'), '--static', '--float-format',
        '--build', str(OUT / 'c16'), '-I', str(PORT),
        '-I', str(ROOT / 'ports/graphics/include'), '-o', str(application),
    ], check=True, cwd=ROOT)
    sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
    from guest import Guest
    with tempfile.TemporaryDirectory(prefix='graphite-render-') as directory:
        machine = Guest(Path(directory), 'native', {'render-check': application},
                        files=[dict(name='/dev/gfx', mode=0o20666, device=0x400)])
        try:
            result = machine.shell('render-check; echo RENDER_CHECK_STATUS:$?', seconds=240)
            assert 'RENDER_CHECK passed' in result and 'RENDER_CHECK_STATUS:0' in result, result
        finally:
            machine.close()
            (OUT / 'guest.log').write_bytes(machine.log)
    subprocess.run([sys.executable, str(PORT / 'render-lvgl-test.py')], check=True, cwd=ROOT)
    lvgl = json.loads((OUT / 'lvgl-result.json').read_text())
    assert lvgl['status'] == 'passed'
    assert digest(kernel) == kernel_hash, 'Kernel changed while the test was running'
    assert {name: digest(ROOT / name) for name in SOURCES} == sources, 'Test inputs changed'
    report = dict(
        status='passed', guest_status='passed', kernel=str(kernel),
        kernel_sha256=kernel_hash, executable_sha256=digest(application),
        input_sha256=sources, lvgl=lvgl,
        checks=[
            'retained targets, bounded lifecycle and opaque stale handles',
            'exact BGR888 and A8 upload/readback including bank crossings and padding',
            'all 256 unmasked fill opacities and 65536 mask/opacity pairs with exact rounding',
            'clipped masks, retained coverage and source subrectangles',
            'fully validated transactional batches and borrowed memory spans',
            'window ownership, fork isolation, teardown and resize',
        ],
    )
    (OUT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Retained GX target lifecycle, pixel parity and request validation passed', flush=True)


if __name__ == '__main__':
    main()
