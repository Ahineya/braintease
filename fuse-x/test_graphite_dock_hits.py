#!/usr/bin/env python3
"""Verify real Dock hover and activation across decorative children."""
import argparse
import importlib.util
import json
from pathlib import Path
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("graphite_shell", ROOT / "fuse-x/test_graphite_shell.py")
shell_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shell_test)


def checks(args):
    ui = shell_test.ShellGUI(args)
    passed, samples = [], {}

    def move(x, y):
        ui.send(0, input=dict(type="motion", x=x, y=y))

    def pixel(image, x, y, scale):
        offset = ((y * scale) * image[0] + x * scale) * 3
        return list(image[2][offset:offset + 3])

    def point(rect, zone):
        x, y, width, height = rect
        return {"background": (x + 8, y + height // 2),
                "icon": (x + width // 2, y + height // 2),
                "indicator": (x + width // 2, y + height - 4)}[zone]

    def hover_path(scale, name):
        rect = ui.items()[shell_test.CALCULATOR]["rect"]
        x, y, _, _ = rect
        move(0, 0)
        ui.save_scene(name + "-rest")
        source, _ = ui.wait(ui.alpha_source)
        rest = pixel(source, x + 10, y + 12, scale)
        colours = {}
        for zone in ("background", "icon", "indicator"):
            move(*point(rect, zone))
            ui.save_scene(name + "-hover-" + zone)
            source, _ = ui.wait(ui.alpha_source)
            colours[zone] = pixel(source, x + 10, y + 12, scale)
        assert colours["background"] != rest, (name, rest, colours)
        assert len({tuple(value) for value in colours.values()}) == 1, (name, colours)
        # Confirm that the last pointer coordinate really covers the painted
        # running indicator, rather than a nearby part of the button.
        indicator = pixel(source, *point(rect, "indicator"), scale)
        assert min(indicator) > 150 and indicator != colours["indicator"], (name, indicator)
        samples[name] = dict(rect=rect, rest=rest, hover=colours, indicator=indicator)
        passed.append(name + ": one continuous hover across button background, icon and running indicator")

    try:
        ui.start_shell()
        ui.app_click(shell_test.CALCULATOR)
        window = ui.painted_calculators(1)[0]["id"]
        ui.wait(lambda: ui.items().get(shell_test.CALCULATOR, {}).get("windows") == 1)
        for scale in (1, 2):
            ui.send(0, density=scale)
            ui.wait(lambda: ui.desktop().get("scale") == scale)
            ui.wait(lambda: next((w["scale"] for w in ui.windows() if w["id"] == ui.shell_window), 0) == scale)
            ui.save_scene(f"dock-ready-{scale}x")
            hover_path(scale, f"focused-{scale}x")
            for zone in ("indicator", "icon", "background"):
                ui.control(window, "minimize")
                ui.wait(lambda: ui.internal(window)["minimized"])
                ui.save_scene(f"minimized-{zone}-{scale}x")
                if zone == "indicator":
                    hover_path(scale, f"unfocused-{scale}x")
                rect = ui.items()[shell_test.CALCULATOR]["rect"]
                ui.click(0, *point(rect, zone))
                ui.wait(lambda: not ui.internal(window)["minimized"] and ui.desktop().get("focused") == window)
                ui.save_scene(f"activated-{zone}-{scale}x")
                passed.append(f"{scale}x: clicking {zone} restores and focuses the owning application")
            ui.click_control("applications", "Applications")
            ui.wait(lambda: ui.panel() and ui.panel()["modal"])
            ui.dismiss()
            passed.append(f"{scale}x: Applications icon activates its owning launcher")
        assert not any(error in ui.g.log for error in
                       (b"native runtime error", b"panic:", b"FUSEDE_ERROR", b"graphics operation failed"))
    finally:
        ui.close()
    return dict(status="passed", checks=passed, samples=samples,
                private_images_only=True, networking="off", private_fsck="passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=ROOT / "build/fusede-run/fuse-x")
    parser.add_argument("--shell", type=Path, default=ROOT / "build/fusede-run/lvgl/fusedeshell.fxe")
    parser.add_argument("--calculator", type=Path, default=ROOT / "build/fusede-run/lvgl/lvcalc.fxe")
    parser.add_argument("--terminal", type=Path, default=ROOT / "build/fusede-shell/gterm.fxe")
    parser.add_argument("--fixture", type=Path, default=ROOT / "build/fusede-run/graphite-tui.fxe")
    parser.add_argument("--catalog", type=Path, default=ROOT / "ports/fusede/catalog")
    parser.add_argument("--build", type=Path, default=ROOT / "fuse-x/build")
    parser.add_argument("--output", type=Path, default=ROOT / "build/fusede-dock-hits/guest-tests")
    args = parser.parse_args()
    for name in vars(args):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    result = args.output / "result.json"
    result.unlink(missing_ok=True)
    started = time.monotonic()
    try:
        report = checks(args)
        report.update(kernel_sha256=shell_test.digest(args.kernel), shell_sha256=shell_test.digest(args.shell),
                      calculator_sha256=shell_test.digest(args.calculator), runner_sha256=shell_test.digest(__file__),
                      elapsed_seconds=round(time.monotonic() - started, 3))
    except BaseException as error:
        result.write_text(json.dumps(dict(status="failed", error=str(error)), indent=2) + "\n")
        raise
    result.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Graphite Dock hit testing: {len(report['checks'])} focused guest groups passed")


if __name__ == "__main__":
    main()
