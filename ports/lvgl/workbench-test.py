#!/usr/bin/env python3
"""Real offline Fuse-X input for the shared table/plot workbench.

Every fixture is generated here and copied into a fresh disposable image. The
geometry trace is test-only, enabled by lvdata --trace; all controls are reached
through actual GX pointer/keyboard/clipboard input.
"""
from pathlib import Path
import hashlib
import importlib.util
import json
import math
import re
import shlex
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
BUILD = ROOT / "build/lvgl"
spec = importlib.util.spec_from_file_location("gui", PORT / "gui-test.py")
gui = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gui)


def fixtures(folder):
    folder.mkdir(exist_ok=True)
    sample = folder / "signals.csv"
    rows = ["Sample,Time,Signal & amplitude,Other"]
    for i in range(40):
        value = "missing" if i in (4, 11, 18) else str(100 if i == 23 else -50 if i == 7 else i % 5)
        rows.append(f"{i + 1},{i / 2:.1f},{value},{40 - i}")
    sample.write_text("\n".join(rows) + "\n")
    large = folder / "large-signals.csv"
    with large.open("w") as stream:
        stream.write(rows[0] + "\n")
        for i in range(100000):
            stream.write(f"{i + 1},{i},{(i * 3571) % 100003},{100000 - i}\n")
    invalid = folder / "invalid-signals.csv"
    invalid.write_text("Name,Value\none,no\ntwo,NaN\nthree,Infinity\nfour,1e999\n")
    long_numbers = folder / "long-numbers.csv"
    long_numbers.write_text("Name,Reading\none," + "0" * 20000 + "12.5\ntwo,-" + "0" * 20000 + "7\n")
    return sample, large, invalid, long_numbers


def main():
    out = BUILD / "workbench-tests"
    out.mkdir(exist_ok=True)
    (out / "result.json").unlink(missing_ok=True)
    sample, large, invalid, long_numbers = fixtures(out / "fixtures")
    paths = (sample, large, invalid, long_numbers)
    files = [dict(name=str(path), path=str(path), mode=0o100644) for path in paths]
    executable_hash = hashlib.sha256((BUILD / "lvdata.fxe").read_bytes()).hexdigest()
    # Hashing from inside the guest proves source preservation without relying
    # on the host fixture, and avoids dumping a 100k-row document into the log.
    checker = out / "data-check.fxe"
    with (out / "checker-build.log").open("w") as log:
        subprocess.run([sys.executable, str(ROOT / "fuse-x/cc.py"), str(PORT / "data-check.c"),
                        "--static", "-I", str(ROOT / "ports/graphics/include"), "-o", str(checker)],
                       cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    x = gui.GUI(out, {"lvdata": BUILD / "lvdata.fxe", "data-check": checker}, files)
    g = x.g
    metrics = {}
    wid = None

    def lines(expression, at=0, seconds=90):
        pattern = re.compile(expression.encode() + rb"\r?\n")
        return x.wait(lambda: pattern.findall(g.log[at:]), seconds)

    def ready(at=0, points=None, valid=None, invalid_count=None, xc=None, yc=None, mode=None):
        matches = lines(r"DATA_PLOT_READY points=(\d+) valid=(\d+) invalid=(\d+) x=(\d+) y=(\d+) mode=(\d+)", at)
        values = tuple(map(int, matches[-1]))
        for expected, actual in zip((points, valid, invalid_count, xc, yc, mode), values):
            if expected is not None:
                assert actual == expected, (values, (points, valid, invalid_count, xc, yc, mode))
        return values

    def geometry(at=0):
        names = ("plot", "x", "y", "mode", "fit", "plus", "minus", "export", "update")
        pattern = re.compile(rb"DATA_PLOT_RECT (\w+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)\r?\n")
        def rectangles():
            result = {}
            for name, a, b, c, d in pattern.findall(g.log[at:]):
                a, b, c, d = map(int, (a, b, c, d))
                result[name.decode()] = [a, b, c - a + 1, d - b + 1]
            return result if all(name in result for name in names) else None
        return x.wait(rectangles)

    def click(name):
        rect = geometry()[name]
        x.click(wid, rect[0] + rect[2] // 2, rect[1] + rect[3] // 2)

    def choose(name, index, rebuild=True):
        click(name)
        # Navigate inside the stock popup and commit exactly once. Incremental
        # jobs may focus their cancellation control after a committed choice.
        x.key(wid, 74)
        for _ in range(index):
            x.key(wid, 81)
        at = len(g.log)
        x.key(wid, 40)
        x.pause(.15)
        if rebuild:
            return ready(at)
        return lines(r"DATA_PLOT_OPTIONS x=(\d+) y=(\d+) mode=(\d+)", at)[-1]

    def set_path(path):
        # The shared file browser enumerates its initial folder cooperatively;
        # submission stays disabled until that small fixture listing settles.
        x.frame(wid)
        x.send(wid, action=4)
        x.send(wid, action=2, clipboard=str(path))

    def open_path(path):
        at = len(g.log)
        x.ctrl(wid, 18)
        lines(r"DATA_DIALOG open", at)
        set_path(path)
        x.key(wid, 40)
        return at

    def launch(path=sample, plot=True):
        nonlocal wid
        at = len(g.log)
        g.shell("lvdata --trace " + ("--plot " if plot else "") + shlex.quote(str(path)) + " &")
        g.shell("viewer=$!")
        wid = x.wait(lambda: x.window("Data Viewer"))["id"]
        lines(r"DATA_OPEN " + re.escape(str(path)) + r" rows=\d+ columns=\d+ delimiter=\d+ widgets=\d+", at)
        ready(at)
        geometry(at)
        x.send(wid, density=1)
        x.frame(wid)
        rects = geometry()
        assert rects["mode"][2] == 104 and rects["plus"][2] == 32 and rects["minus"][2] == 32, rects
        return at

    def close():
        x.send(wid, close=True)
        x.wait(lambda: not x.window("Data Viewer"))
        assert "STATUS:0" in g.shell("wait $viewer; echo STATUS:$?")

    def hash_file(path):
        text = g.shell("data-check " + shlex.quote(str(path)))
        result = re.search(r"DATA_CHECK (\d+) ([0-9a-f]{8})", text)
        assert result, text
        return tuple(result.groups())

    def svg(path):
        text = g.shell("cat " + shlex.quote(path))
        start, end = text.find("<?xml"), text.find("</svg>")
        assert start >= 0 and end >= 0, text[-2000:]
        xml = text[start:end + len("</svg>")]
        document = ET.fromstring(xml)
        assert document.tag == "{http://www.w3.org/2000/svg}svg"
        assert "Signal & amplitude" in "".join(document.itertext())
        for node in document.iter():
            for name in ("x", "y", "cx", "cy", "r"):
                if name in node.attrib:
                    assert math.isfinite(float(node.attrib[name])), node.attrib
        return document, hashlib.sha256(xml.encode()).hexdigest()

    def export_dialog(path):
        at = len(g.log)
        x.ctrl(wid, 8)
        lines(r"DATA_PLOT_EXPORT_OPEN", at)
        set_path(path)

    def plot_pixels(frame, rect, scale):
        width, height, pixels = frame
        left, top = (rect[0] + 3) * scale, (rect[1] + 3) * scale
        right, bottom = (rect[0] + rect[2] - 3) * scale, (rect[1] + rect[3] - 3) * scale
        assert 0 <= left < right <= width and 0 <= top < bottom <= height
        return b"".join(pixels[(y * width + left) * 3:(y * width + right) * 3]
                        for y in range(top, bottom))

    try:
        source_hashes = {str(path): hash_file(path) for path in paths}
        launch()
        choose("x", 2)
        choose("y", 2)
        ready(points=40, valid=37, invalid_count=3, xc=2, yc=2, mode=0)
        # The same logical controls work at both backing densities. Fit is
        # reversible after real pointer drag, toolbar zoom and keyboard pan.
        for scale in (1, 2):
            at = len(g.log)
            x.send(wid, density=scale)
            x.frame(wid, scale=scale)
            rects = geometry()
            assert rects["mode"][2] == 104 and rects["plus"][2] == 32 and rects["minus"][2] == 32, rects
            rect = rects["plot"]
            center = (rect[0] + rect[2] // 2, rect[1] + rect[3] // 2)
            x.click(wid, *center)  # clear the initial selected-row marker
            click("fit")
            baseline = plot_pixels(x.frame(wid, scale=scale), rect, scale)
            x.snapshot(wid, f"line-{scale}x", scale=scale)
            x.send(wid, input=dict(type="button", button=0, down=True, x=center[0], y=center[1]))
            x.send(wid, input=dict(type="motion", x=center[0] + 30, y=center[1] + 12))
            x.send(wid, input=dict(type="button", button=0, down=False, x=center[0] + 30, y=center[1] + 12))
            assert plot_pixels(x.frame(wid, scale=scale), rect, scale) != baseline, "Pointer drag did not pan the plot"
            click("fit")
            assert plot_pixels(x.frame(wid, scale=scale), rect, scale) == baseline, "Fit did not restore the original view"
            click("plus")
            assert plot_pixels(x.frame(wid, scale=scale), rect, scale) != baseline, "Pointer zoom did not change the view"
            click("fit")
            assert plot_pixels(x.frame(wid, scale=scale), rect, scale) == baseline
            x.click(wid, *center)
            x.key(wid, 79)
            assert plot_pixels(x.frame(wid, scale=scale), rect, scale) != baseline, "Keyboard pan did not change the view"
            x.key(wid, 74)
            assert plot_pixels(x.frame(wid, scale=scale), rect, scale) == baseline
        x.send(wid, density=1)
        x.frame(wid)
        choose("mode", 1, rebuild=False)
        assert tuple(map(int, lines(r"DATA_PLOT_OPTIONS x=(\d+) y=(\d+) mode=(\d+)")[-1])) == (2, 2, 1)
        x.snapshot(wid, "scatter")
        # Sort by numeric Time descending and select the known extremum in the
        # plot. Its table row changes; the original CSV source identity does not.
        for expected in (0, 1):
            at = len(g.log)
            x.click(wid, 310, 134)
            lines(r"DATA_SORT 1 " + str(expected) + r" first=[^\r\n]+", at)
            ready(at, points=40, valid=37, invalid_count=3, xc=2, yc=2, mode=1)
        click("fit")
        rect = geometry()["plot"]
        xmin, xmax, ymin, ymax = -0.6825, 20.1825, -55.25, 105.25
        px = rect[0] + 71 + (11.5 - xmin) / (xmax - xmin) * (rect[2] - 89)
        py = rect[1] + 31 + (ymax - 100) / (ymax - ymin) * (rect[3] - 81)
        at = len(g.log)
        x.click(wid, round(px), round(py))
        lines(r"DATA_PLOT_SELECT source=24 row=16", at)
        lines(r"DATA_SELECT 16 2", at)
        x.snapshot(wid, "linked-sorted-selection")

        path = "/data/home/workbench.svg"
        export_dialog(path)
        at = len(g.log)
        x.key(wid, 40)
        lines(r"DATA_PLOT_EXPORT " + re.escape(path), at)
        document, first_export = svg(path)
        assert document.findall(".//{http://www.w3.org/2000/svg}circle")
        exported_hash = hash_file(path)
        # Finishing a job must not strand keyboard focus on its now-hidden
        # Cancel button. The completed plot accepts navigation immediately.
        rect = geometry()["plot"]
        baseline = plot_pixels(x.frame(wid), rect, 1)
        x.key(wid, 79)
        assert plot_pixels(x.frame(wid), rect, 1) != baseline, "Export left focus on hidden progress"
        x.key(wid, 74)
        assert plot_pixels(x.frame(wid), rect, 1) == baseline
        # First Enter requests replacement; cancelling must preserve the file.
        export_dialog(path)
        at = len(g.log)
        x.key(wid, 40)
        x.pause(.2)
        assert b"DATA_PLOT_EXPORT " not in g.log[at:]
        assert hash_file(path) == exported_hash
        x.snapshot(wid, "replace-confirmation")
        x.key(wid, 41)
        assert hash_file(path) == exported_hash
        # Invalid suffix is reported in the still-open modal; retry works.
        export_dialog("/data/home/not-an-svg.txt")
        at = len(g.log)
        x.key(wid, 40)
        x.pause(.2)
        assert b"DATA_PLOT_EXPORT " not in g.log[at:]
        x.snapshot(wid, "export-validation")
        set_path(path)
        x.key(wid, 40)
        at = len(g.log)
        x.key(wid, 40)
        lines(r"DATA_PLOT_EXPORT " + re.escape(path), at)
        svg(path)

        close()
        settings = g.shell("cat /data/home/.lvdata")
        for text in ("plot=1", "plot_x=2", "plot_y=2", "plot_mode=1"):
            assert text in settings, settings
        launch(plot=False)
        ready(points=40, valid=37, invalid_count=3, xc=2, yc=2, mode=1)
        at = len(g.log)
        x.ctrl(wid, 19)
        lines(r"DATA_PLOT_SPLIT \d+ visible=0", at)
        at = len(g.log)
        x.ctrl(wid, 19)
        lines(r"DATA_PLOT_SPLIT \d+ visible=1", at)

        # Work begins asynchronously with a focused cancellation control.
        # Enter queues cancellation through the same shared progress widget.
        at = open_path(large)
        lines(r"DATA_WORK_START \d+", at)
        x.key(wid, 41)
        lines(r"DATA_CANCELLED", at)
        assert not re.search(rb"DATA_OPEN " + re.escape(str(large).encode()) + rb" rows=", g.log[at:])
        at = open_path(large)
        start = time.monotonic()
        lines(r"DATA_OPEN " + re.escape(str(large)) + r" rows=100000 columns=4 delimiter=44 widgets=\d+", at, 180)
        ready(at, points=100000, valid=100000, invalid_count=0, xc=2, yc=2, mode=1)
        metrics["large_open_and_plot_seconds"] = time.monotonic() - start
        x.snapshot(wid, "large-scatter")
        at = len(g.log)
        click("update")
        x.key(wid, 41)
        lines(r"DATA_PLOT_CANCEL", at)
        at = len(g.log)
        click("update")
        ready(at, points=100000, valid=100000, invalid_count=0, xc=2, yc=2, mode=1)
        # Stream a large viewport export, cancel after it starts, and prove
        # replacement never occurred and the private temporary was removed.
        exported_hash = hash_file(path)
        export_dialog(path)
        x.key(wid, 40)  # explicit replacement confirmation
        at = len(g.log)
        x.key(wid, 40)
        lines(r"DATA_PLOT_EXPORT_BEGIN " + re.escape(path), at)
        x.key(wid, 41)
        lines(r"DATA_PLOT_EXPORT_CANCEL", at)
        assert hash_file(path) == exported_hash
        assert "workbench.svg.plot-*.tmp" in g.shell("echo /data/home/workbench.svg.plot-*.tmp")
        for scale, width, height in ((1, 1470, 900), (2, 1470, 900), (1, 800, 560)):
            x.send(wid, density=scale)
            x.send(wid, resize=[width, height])
            x.frame(wid, scale=scale, width=width, height=height)
            click("plus")
            click("fit")
        # Automatic Y selection skips a text column. Long but valid numeric
        # tokens are scanned incrementally, rather than rejected or blocked.
        at = open_path(long_numbers)
        lines(r"DATA_OPEN " + re.escape(str(long_numbers)) + r" rows=2 columns=2 delimiter=44 widgets=\d+", at)
        ready(at, points=2, valid=2, invalid_count=0, xc=2, yc=1, mode=1)
        # Non-finite tokens remain explicitly invalid samples and cannot be exported.
        at = open_path(invalid)
        lines(r"DATA_OPEN " + re.escape(str(invalid)) + r" rows=4 columns=2 delimiter=44 widgets=\d+", at)
        ready(at, points=4, valid=0, invalid_count=4)
        x.snapshot(wid, "no-numeric-samples")
        at = len(g.log)
        x.ctrl(wid, 8)
        x.pause(.2)
        assert b"DATA_PLOT_EXPORT_OPEN" not in g.log[at:], "Disabled export shortcut ran"
        at = open_path(large)
        lines(r"DATA_WORK_START \d+", at)
        close()  # discard an active loader and its incremental buffers
        assert not re.search(rb"DATA_OPEN " + re.escape(str(large).encode()) + rb" rows=", g.log[at:])
        # Close again after the document commits but while its 100k-sample plot
        # is being prepared. Timers and borrowed model mappings must be released
        # before the application destroys its table/window.
        at = len(g.log)
        g.shell("lvdata --trace --plot " + shlex.quote(str(large)) + " &")
        g.shell("viewer=$!")
        wid = x.wait(lambda: x.window("Data Viewer"))["id"]
        lines(r"DATA_OPEN " + re.escape(str(large)) + r" rows=100000 columns=4 delimiter=44 widgets=\d+", at, 180)
        close()
        assert b"DATA_PLOT_READY " not in g.log[at:], "Plot finished before the close-during-preparation check"
        for file in paths:
            assert hash_file(file) == source_hashes[str(file)], "A source document was rewritten"
        assert not any(s in g.log for s in (b"native runtime error", b"panic:", b"graphics operation failed"))
        assert executable_hash == hashlib.sha256((BUILD / "lvdata.fxe").read_bytes()).hexdigest(), "Executable changed during testing"
        report = dict(status="passed", executable_sha256=executable_hash,
                      metrics=metrics, svg_sha256=first_export, checks=[
                          "real shared column choices, line/scatter and fit controls",
                          "pointer pan, toolbar zoom and keyboard navigation at1x/2x",
                          "known peak retains original source identity after numeric sorting",
                          "standalone XML-valid SVG with escaped labels and finite coordinates",
                          "path validation and explicit replacement/cancellation",
                          "keyboard focus restored after incremental export",
                          "plot settings survive process restart",
                          "incremental large-file cancellation retains existing document",
                          "plot preparation and streaming export cancellation with temporary cleanup",
                          "100000-sample plot at fullscreen and both backing densities",
                          "automatic numeric-column detection and long finite numeric tokens",
                          "invalid/nonfinite samples disable export",
                          "source files preserved; normal close during active loading and plot preparation"])
        (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        print("Workbench: real table/plot controls, source-linked selection, SVG, settings and incremental jobs passed")
    finally:
        x.close()


if __name__ == "__main__":
    main()
