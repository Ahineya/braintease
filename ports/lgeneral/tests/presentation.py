#!/usr/bin/env python3
"""Check native presentation zoom and cursor composition in a private Fuse-X."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile
import time

from gui import Game, ROOT, png, region


DESCRIPTION = (330, 190, 190, 200)


def differences_outside_cursors(frame, baseline, positions):
    """Compare dialog pixels while allowing legitimate old/new bitmap cursors."""
    x, y, width, height = DESCRIPTION
    pitch = frame[0] * 3
    count = 0
    for row in range(y, y + height):
        spans = []
        for px, py in positions:
            if py <= row < py + 22 and px < x + width and px + 22 > x:
                spans.append((max(x, px), min(x + width, px + 22)))
        current = x
        for first, last in sorted(spans) + [(x + width, x + width)]:
            if first > current:
                start, end = row * pitch + current * 3, row * pitch + first * 3
                actual, expected = frame[2][start:end], baseline[2][start:end]
                if actual != expected:
                    count += sum(actual[i:i + 3] != expected[i:i + 3] for i in range(0, len(actual), 3))
            current = max(current, last)
    return count


def metadata(game, zoom, width=800, height=600):
    def ready():
        window = game.window()
        if not window:
            return None
        expected = dict(width=width, height=height, scale=1,
                        pixel_width=width, pixel_height=height, zoom_percent=zoom,
                        presentation_width=(width * zoom + 50) // 100,
                        presentation_height=(height * zoom + 50) // 100)
        return window if all(window.get(name) == value for name, value in expected.items()) else None
    return game.wait(ready, message=f"{zoom}% presentation at logical {width}x{height}")


def start(game, override=None):
    if override is not None:
        game.g.shell(f"SDL_GX_WINDOW_ZOOM={override}; export SDL_GX_WINDOW_ZOOM")
    at = len(game.g.log)
    game.g.shell("lgeneral &")
    game.g.shell("game=$!")
    game.wait(lambda: b"LGENERAL READY title" in game.g.log[at:], message="title initialized")
    return metadata(game, override or 150)


def campaign(game, output, name):
    before = game.screenshot(output, name + "-title")
    game.menu(3)
    game.changed(before)
    opened = game.screenshot(output, name + "-selector")
    game.click(243, 200)
    game.changed(opened, (328, 180, 220, 190))
    return game.screenshot(output, name + "-selected")


def cursor_regression(game, output):
    game.event(input=dict(type="motion", x=100, y=100))
    game.pause(.25)
    baseline = game.screenshot(output, "dialog-baseline")
    samples = []
    previous = (100, 100)
    for mode, row in (("hover", 240), ("drag", 300)):
        points = [(x, row) for x in range(340, 481, 20)]
        if mode == "drag":
            game.event(input=dict(type="motion", x=points[0][0], y=points[0][1]))
            game.event(input=dict(type="button", button=0, down=True, x=points[0][0], y=points[0][1]))
        for index, position in enumerate(points):
            game.event(input=dict(type="motion", x=position[0], y=position[1]))
            deadline = time.monotonic() + .16
            sequence = None
            observed = 0
            while time.monotonic() < deadline:
                game.g.pump()
                window = game.window()
                frame = game.frame()
                if not frame or not window or sequence == window["sequence"]:
                    continue
                sequence = window["sequence"]
                unexpected = differences_outside_cursors(frame, baseline, (previous, position))
                if unexpected:
                    (output / f"cursor-failure-{mode}-{index}.png").write_bytes(png(frame))
                assert unexpected == 0, f"{mode} at {position}: {unexpected} dialog pixels changed outside cursor sprites"
                observed += 1
            samples.append(dict(mode=mode, position=list(position), frames=observed, unexpected_pixels=0))
            previous = position
        if mode == "drag":
            game.event(input=dict(type="button", button=0, down=False, x=previous[0], y=previous[1]))
        frame = game.screenshot(output, "dialog-" + mode)
        assert region(frame, *previous, 22, 22) != region(baseline, *previous, 22, 22), "game bitmap cursor disappeared"
    game.event(input=dict(type="motion", x=100, y=100))
    restored = game.screenshot(output, "dialog-restored")
    assert region(restored, *DESCRIPTION) == region(baseline, *DESCRIPTION), "pointer left permanent dialog damage"
    assert sum(sample["frames"] for sample in samples) > 0, "no presented motion frames sampled"
    return samples


def stop(game):
    game.event(input=dict(type="close"))
    game.wait(lambda: not game.window(), message="closed window")
    result = game.g.shell("wait $game; echo PRESENTATION_EXIT:$?")
    assert "PRESENTATION_EXIT:0" in result, result


def run(binary: Path, data: Path, kernel: Path, output: Path):
    binary, data, kernel, output = (Path(p).resolve() for p in (binary, data, kernel, output))
    for path in (binary, kernel, data / "assets-manifest.json"):
        if not path.is_file():
            raise FileNotFoundError(path)
    output.mkdir(parents=True, exist_ok=True)
    report = dict(executable_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  kernel_sha256=hashlib.sha256(kernel.read_bytes()).hexdigest(), checks=[], metadata=[])
    with tempfile.TemporaryDirectory(prefix="lgeneral-presentation-", dir=output) as directory:
        old_kernel = os.environ.get("FUSE_X_TEST_KERNEL")
        old_zoom = os.environ.pop("SDL_GX_WINDOW_ZOOM", None)
        os.environ["FUSE_X_TEST_KERNEL"] = str(kernel)
        try:
            game = Game(binary, data, Path(directory))
        finally:
            if old_kernel is None:
                os.environ.pop("FUSE_X_TEST_KERNEL", None)
            else:
                os.environ["FUSE_X_TEST_KERNEL"] = old_kernel
            if old_zoom is not None:
                os.environ["SDL_GX_WINDOW_ZOOM"] = old_zoom
        try:
            report["metadata"].append(start(game))
            assert game.frame()[:2] == (800, 600), "zoom enlarged the guest raster"
            report["checks"].append("default 150% native presentation preserves 800x600 guest raster and backing scale 1")
            campaign(game, output, "default")
            report["cursor_samples"] = cursor_regression(game, output)
            report["checks"].append("translucent campaign dialog survives hover and held-button drag without cursor rectangle publication")
            for width, height in ((1024, 768), (800, 600)):
                game.event(resize=[width, height])
                report["metadata"].append(metadata(game, 150, width, height))
                game.wait(lambda: game.sidebar_ready(width, height), message="fully composed resized sidebar")
                game.screenshot(output, f"default-{width}x{height}")
                if width == 1024:
                    # Dialog follows the guest's new coordinates, independent
                    # of native 150% presentation and physical display density.
                    before = game.frame()
                    x, y = (width - 44 - 340) // 2, (height - 240) // 2
                    game.click(x + 319, y + 220)  # Cancel at its new location.
                    game.changed(before, (x, y, 340, 240))
                    game.screenshot(output, "default-resized-cancel")
            campaign(game, output, "default-restored-input")
            report["checks"].append("1024x768 and 800x600 reflow retain zoom and logical pointer targeting")
            stop(game)
            report["metadata"].append(start(game, 100))
            campaign(game, output, "override100")
            report["checks"].append("explicit SDL_GX_WINDOW_ZOOM=100 overrides the application default with unchanged logical input")
            stop(game)
            for bad in (b"panic:", b"runtime error:", b"NXR_BUFFER_FAILED", b"assertion failed"):
                assert bad not in game.g.log, game.g.log[-6000:]
            report["status"] = "passed"
        except BaseException as error:
            report["status"] = "failed"
            report["error"] = str(error)
            frame = game.frame()
            if frame:
                (output / "failure.png").write_bytes(png(frame))
            raise
        finally:
            (output / "guest.log").write_bytes(game.g.log)
            (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
            game.g.close()
    report["checks"].append("both instances close successfully and the private 128MiB root passes filesystem checking")
    report["screenshots"] = [p.name for p in sorted(output.glob("*.png"))]
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/lgeneral/lgeneral.fxe")
    parser.add_argument("--data", type=Path, default=ROOT / "build/lgeneral/data")
    parser.add_argument("--kernel", type=Path, default=ROOT / "fuse-x/build/fuse-x")
    parser.add_argument("--output", type=Path, default=ROOT / "build/lgeneral-window/presentation")
    args = parser.parse_args()
    print(json.dumps(run(args.binary, args.data, args.kernel, args.output), indent=2))
