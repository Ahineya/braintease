#!/usr/bin/env python3
"""Gallery plot/form interactions in a fresh offline Fuse-X test image."""
from pathlib import Path
import hashlib
import importlib.util
import json
import re

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
BUILD = ROOT / 'build/lvgl'
spec = importlib.util.spec_from_file_location('gui', PORT / 'gui-test.py')
gui = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gui)


def main():
    out = BUILD / 'gallery-plot-tests'
    out.mkdir(exist_ok=True)
    (out / 'result.json').unlink(missing_ok=True)
    fixture = out / 'existing.txt'
    fixture.write_text('The gallery must never replace this file.\n')
    app = BUILD / 'lvgallery.fxe'
    x = gui.GUI(out, {'lvgallery': app}, [dict(name='/data/forms-existing.txt', path=str(fixture), mode=0o100644)])
    g = x.g
    curve_rounding = {}

    def rect(name):
        matches = re.findall(rb'GALLERY_RECT ' + name.encode() + rb' (-?\d+) (-?\d+) (-?\d+) (-?\d+)\r?\n', g.log)
        assert matches, name
        return tuple(map(int, matches[-1]))

    def click(name):
        a, b, c, d = rect(name)
        x.click(wid, (a + c) // 2, (b + d) // 2)

    def choose(name, key, marker):
        click(name)
        at = len(g.log)
        x.key(wid, key)
        assert marker.encode() not in g.log[at:], 'Choice changed before confirmation'
        x.key(wid, 40)
        x.logged(marker, at)

    def type_path(path):
        # Native host editing commands arrive as GX_ACTION, independently of
        # raw USB keys used for application shortcuts in this harness.
        x.send(wid, action=4)
        x.text(wid, path)

    def reference(name, scale):
        expected = gui.ppm(BUILD / 'host' / f'gallery-{name}-{scale}x.ppm')
        x.frame(wid, scale=scale, width=760)
        def matches():
            actual = gui.ppm(x.frames / f'{wid}.ppm')
            if not actual or actual[:2] != expected[:2]:
                return False
            if actual == expected:
                if name == 'plots':
                    curve_rounding[str(scale)] = 0
                return True
            if name != 'plots':
                return False
            # The C16 and host libm sine demo can round a few antialiased
            # curve pixels differently. Controls, axes, labels and layout
            # remain exact; permit only a tiny, local one-pixel curve delta.
            width, height, pixels = actual
            reference_pixels = expected[2]
            differences = sorted({i // 3 for i, (a, b) in enumerate(zip(pixels, reference_pixels)) if a != b})
            if len(differences) > 64 * scale * scale:
                return False
            a, b, c, d = rect('plot')
            left, top, right, bottom = (a + 71) * scale, (b + 31) * scale, (c - 17) * scale, (d - 49) * scale
            def curve(data, index):
                r, g, blue = data[index * 3:index * 3 + 3]
                return blue - g >= 7 and g - r >= 3
            def nearby(data, x, y):
                return any(curve(data, yy * width + xx)
                           for yy in range(max(top, y - 1), min(bottom, y + 1) + 1)
                           for xx in range(max(left, x - 1), min(right, x + 1) + 1))
            for index in differences:
                pixel_x, pixel_y = index % width, index // width
                if not (left <= pixel_x <= right and top <= pixel_y <= bottom):
                    return False
                offset = index * 3
                if max(abs(pixels[offset + i] - reference_pixels[offset + i]) for i in range(3)) <= 1:
                    continue
                if curve(pixels, index) and not nearby(reference_pixels, pixel_x, pixel_y):
                    return False
                if curve(reference_pixels, index) and not nearby(pixels, pixel_x, pixel_y):
                    return False
                if not curve(pixels, index) and not curve(reference_pixels, index):
                    return False
            curve_rounding[str(scale)] = len(differences)
            return True
        try:
            x.wait(matches, seconds=10)
        finally:
            (out / f'{name}-actual-{scale}x.ppm').write_bytes((x.frames / f'{wid}.ppm').read_bytes())

    try:
        g.shell('lvgallery --trace &')
        g.shell('gallery=$!')
        wid = x.wait(lambda: x.window('Graphite Gallery'))['id']
        at = len(g.log)
        x.key(wid, 64)
        x.logged('GALLERY_PLOT_READY count=1000 valid=960 peak=700', at)
        x.logged('GALLERY_PAGE 6', at)
        for scale in (1, 2):
            x.send(wid, density=scale)
            reference('plots', scale)
            x.snapshot(wid, f'plots-{scale}x', scale=scale, width=760)
        x.send(wid, density=1)
        x.frame(wid, width=760)
        choose('plot-mode', 77, 'GALLERY_PLOT_MODE 1')
        # Escape preserves the selected value, even after keyboard navigation.
        click('plot-mode')
        at = len(g.log)
        x.key(wid, 74)
        x.key(wid, 41)
        assert b'GALLERY_PLOT_MODE ' not in g.log[at:]
        choose('plot-mode', 74, 'GALLERY_PLOT_MODE 0')
        for name, command in (('plot-zoom-in', 1), ('plot-zoom-out', 2), ('plot-fit', 0)):
            at = len(g.log)
            click(name)
            x.logged(f'GALLERY_PLOT_COMMAND {command}', at)
        # The isolated peak must retain source row 700 after viewport sampling.
        a, b, c, d = rect('plot')
        left, top, right, bottom = a + 71, b + 31, c - 17, d - 49
        peak_x = round(left + (70 + 99.9 * .035) / (99.9 * 1.07) * (right - left))
        peak_y = round(top + .035 / 1.07 * (bottom - top))
        at = len(g.log)
        x.click(wid, peak_x, peak_y)
        x.logged('GALLERY_PLOT_SELECT 700', at)
        x.snapshot(wid, 'peak-selected', width=760)
        choose('plot-state', 77, 'GALLERY_PLOT_STATE 2')
        x.snapshot(wid, 'plot-nonnumeric', width=760)
        choose('plot-state', 74, 'GALLERY_PLOT_STATE 0')
        click('plot-state')
        x.key(wid, 81)
        at = len(g.log)
        x.key(wid, 40)
        x.logged('GALLERY_PLOT_STATE 1', at)
        x.snapshot(wid, 'plot-empty', width=760)
        choose('plot-state', 74, 'GALLERY_PLOT_STATE 0')
        for width, height, scale in ((640, 460, 1), (1470, 900, 2), (760, 560, 1)):
            x.send(wid, density=scale)
            x.send(wid, resize=[width, height])
            x.frame(wid, width=width, height=height, scale=scale)

        at = len(g.log)
        x.key(wid, 65)
        x.logged('GALLERY_FORMS_READY choices=65 samples=8388608', at)
        for scale in (1, 2):
            x.send(wid, density=scale)
            reference('forms', scale)
            x.snapshot(wid, f'forms-{scale}x', scale=scale, width=760)
        x.send(wid, density=1)
        x.frame(wid, width=760)
        choose('forms-choice', 77, 'GALLERY_CHOICE 64')
        click('forms-choice')
        at = len(g.log)
        x.key(wid, 74)
        x.key(wid, 41)
        assert b'GALLERY_CHOICE ' not in g.log[at:]

        at = len(g.log)
        click('forms-open')
        x.logged('GALLERY_PATH_OPEN open', at)
        type_path('/data/no-such-file.txt')
        x.key(wid, 40)
        at = len(g.log)
        x.key(wid, 64)
        assert b'GALLERY_PAGE ' not in g.log[at:], 'Modal leaked a page shortcut'
        x.snapshot(wid, 'open-validation', width=760)
        type_path('/data/forms-existing.txt')
        at = len(g.log)
        x.key(wid, 40)
        x.logged('GALLERY_PATH_ACCEPT open /data/forms-existing.txt', at)

        at = len(g.log)
        click('forms-save')
        x.logged('GALLERY_PATH_OPEN save', at)
        type_path('/data/forms-existing.txt')
        at = len(g.log)
        x.key(wid, 40)
        x.pause()
        assert b'GALLERY_PATH_ACCEPT ' not in g.log[at:], 'Overwrite was not confirmed'
        x.snapshot(wid, 'overwrite-confirmation', width=760)
        x.key(wid, 40)
        x.logged('GALLERY_PATH_ACCEPT save /data/forms-existing.txt', at)
        assert 'The gallery must never replace this file.' in g.shell('cat /data/forms-existing.txt')

        at = len(g.log)
        click('forms-open')
        x.logged('GALLERY_PATH_OPEN open', at)
        x.key(wid, 41)
        x.logged('GALLERY_PATH_CANCEL open', at)

        at = len(g.log)
        click('forms-start')
        x.logged('GALLERY_JOB_START 8388608', at)
        # Starting a bounded job deliberately focuses its available Cancel.
        x.key(wid, 40)
        x.logged('GALLERY_JOB_CANCEL ', at)
        at = len(g.log)
        click('forms-start')
        x.logged('GALLERY_JOB_START 8388608', at)
        x.key(wid, 58)
        x.logged('GALLERY_PAGE 0', at)
        x.pause(.5)
        assert b'GALLERY_JOB_DONE ' not in g.log[at:], 'Page replacement left a job alive'
        at = len(g.log)
        x.key(wid, 65)
        x.logged('GALLERY_FORMS_READY ', at)
        at = len(g.log)
        click('forms-start')
        x.logged('GALLERY_JOB_DONE 8388608 checksum=', at)
        x.snapshot(wid, 'job-complete', width=760)
        for key in (64, 65, 64, 58):
            x.key(wid, key)
        x.send(wid, input=dict(type='motion', x=750, y=550))
        x.frame(wid, width=760)
        stamp = (x.frames / f'{wid}.ppm').stat().st_mtime_ns
        x.pause(1.1)
        assert (x.frames / f'{wid}.ppm').stat().st_mtime_ns == stamp, 'Idle gallery kept publishing'
        x.send(wid, close=True)
        x.wait(lambda: not x.window('Graphite Gallery'))
        assert 'STATUS:0' in g.shell('wait $gallery; echo STATUS:$?')
        assert not any(s in g.log for s in (b'native runtime error', b'panic:', b'graphics operation failed'))
        report = dict(status='passed', executable_sha256=hashlib.sha256(app.read_bytes()).hexdigest(), curve_rounding_pixels=curve_rounding, checks=[
            'Plots/Forms pages preserve old page indices',
            '1x/2x and narrow/wide window rendering',
            'exact host/guest controls/axes and Forms at 1x/2x; tightly bounded one-pixel sine-curve rounding',
            'line/scatter, fit/zoom, source-row peak selection and missing-data states',
            '65-item choice confirmation and Escape',
            'Open validation, modal shortcut isolation and cancellation',
            'Save replacement confirmation without file writes',
            'real bounded job cancellation, restart and completion',
            'page replacement cancels jobs and releases borrowed plot models',
            'idle publication and normal window teardown'])
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print('Gallery Plots/Forms: pointer, keyboard, paths, bounded work and cleanup passed')
    finally:
        x.close()


if __name__ == '__main__':
    main()
