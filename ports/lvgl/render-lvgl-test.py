#!/usr/bin/env python3
"""Compare retained and software LVGL strips, including mixed-image fallback."""
from pathlib import Path
import hashlib
import importlib.util
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
OUT = ROOT / 'build/lvgl/render-target-tests'
sys.path.insert(0, str(PORT))
spec = importlib.util.spec_from_file_location("gui", PORT / "gui-test.py")
gui_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gui_module)
GUI = gui_module.GUI


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run():
    OUT.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('plot_benchmark', PORT / 'plot-drag-test.py')
    benchmark = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(benchmark)
    application = OUT / 'render-lvgl-check.fxe'
    benchmark.build_workload(application, workload_source=PORT / 'render-lvgl-check.c')
    executable_hash = sha(application)
    cases = []
    gui = GUI(OUT / 'scenes', {'render-scene': application}, [])
    try:
        for scale in (1, 2):
            for scene in ('native', 'mixed'):
                frames = {}
                for path in ('target', 'software'):
                    at = len(gui.g.log)
                    fallback = 'LVGL_SOFTWARE_TARGET=1 ' if path == 'software' else ''
                    gui.g.shell(f'LVGL_PROFILE=1 LVGL_SCALE={scale} {fallback}render-scene {scene} &')
                    gui.g.shell('scene=$!')
                    gui.logged('RENDER_SCENE ready', at=at)
                    window = gui.wait(lambda: gui.window('Retained renderer scene'))
                    wid = window['id']
                    frame = gui.frame(wid, scale=scale, width=800, height=560)
                    name = f'{scene}-{path}-{scale}x'
                    frames[path] = frame
                    gui.snapshot(wid, name, scale=scale, width=800, height=560)
                    gui.send(wid, close=True)
                    gui.wait(lambda: re.search(rb'LVGL_PROFILE [^\r\n]*target_mask_hits=\d+\r?\n', gui.g.log[at:]))
                    result = gui.g.shell('wait $scene; echo SCENE_STATUS:$?')
                    assert 'SCENE_STATUS:0' in result, result
                    gui.wait(lambda: not gui.window('Retained renderer scene'))
                    text = gui.g.log[at:].decode(errors='replace')
                    lines = [line for line in text.splitlines() if line.startswith('LVGL_PROFILE ')]
                    assert len(lines) == 1, text
                    profile = {key: int(value) for key, value in re.findall(r'(\w+)=(\d+)', lines[0])}
                    if path == 'target':
                        assert profile['target_batches'] > 0 and profile['target_commands'] > 0, profile
                        if scene == 'native':
                            assert profile['target_fallbacks'] == profile['target_readbacks'] == profile['rgb_uploads'] == 0, profile
                        else:
                            assert profile['target_fallbacks'] >= 2 and profile['target_readbacks'] >= 2 and profile['rgb_uploads'] >= 2, profile
                    else:
                        assert profile['target_batches'] == profile['target_commands'] == profile['target_readbacks'] == 0, profile
                        assert profile['rgb_uploads'] > 0, profile
                    cases.append(dict(scene=scene, path=path, density=scale, profile=profile))
                assert frames['target'] == frames['software'], f'Native/software pixels differ for {scene} at {scale}x'
        assert not any(error in gui.g.log for error in (b'native runtime error', b'panic:', b'Assertion `', b'graphics operation failed'))
    finally:
        gui.close()
    assert executable_hash == sha(application), 'Scene test executable changed'
    report = dict(status='passed', executable_sha256=executable_hash, cases=cases,
                  checks=['exact retained/software scene equality at 1x/2x',
                          'zero RGB upload and zero readback for native fills/masks',
                          'ordinary RGB888 image replay across multiple strips',
                          'earlier and later fills/text preserved through CPU fallback'])
    (OUT / 'lvgl-result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Retained LVGL strips and mixed RGB image fallback compare exactly at 1x/2x', flush=True)
    return report


if __name__ == '__main__':
    run()
