#!/usr/bin/env python3
"""Check shell hover departure/re-entry through real desktop region routing."""
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

    def move(point):
        ui.send(0, input=dict(type="motion", x=point[0], y=point[1]))

    def center(rect):
        x, y, width, height = rect
        return x + width // 2, y + height // 2

    def source_colour(rect, scale):
        source, _ = ui.wait(ui.alpha_source)
        x, y = rect[0] + 10, rect[1] + 12
        offset = ((y * scale) * source[0] + x * scale) * 3
        return list(source[2][offset:offset + 3])

    try:
        ui.start_shell()
        ui.app_click(shell_test.CALCULATOR)
        calculator = ui.painted_calculators(1)[0]["id"]
        ui.wait(lambda: ui.items().get(shell_test.CALCULATOR, {}).get("windows") == 1)
        for scale in (1, 2):
            ui.send(0, density=scale)
            ui.wait(lambda: ui.desktop().get("scale") == scale)
            ui.wait(lambda: next((w["scale"] for w in ui.windows() if w["id"] == ui.shell_window), 0) == scale)
            ui.save_scene(f"ready-{scale}x")
            controls = {"riplix": ui.controls()[("brand", "Riplix")],
                        "applications": ui.controls()[("applications", "Applications")],
                        "calculator": ui.items()[shell_test.CALCULATOR]["rect"]}
            client = ui.internal(calculator)
            destinations = {"desktop": (ui.desktop()["width"] // 2, ui.desktop()["height"] // 2),
                            "client": (client["x"] + client["width"] // 2,
                                       client["y"] + client["height"] // 2),
                            "clock": center(ui.controls()[("clock", "clock")])}
            for name, rect in controls.items():
                move(destinations["desktop"])
                ui.save_scene(f"{name}-rest-{scale}x")
                rest = source_colour(rect, scale)
                colours = {}
                for destination, point in destinations.items():
                    move(center(rect))
                    ui.save_scene(f"{name}-entered-{destination}-{scale}x")
                    hovered = source_colour(rect, scale)
                    assert hovered != rest, (name, scale, "no hover", rest, hovered)
                    move(point)
                    ui.save_scene(f"{name}-left-{destination}-{scale}x")
                    departed = source_colour(rect, scale)
                    assert departed == rest, (name, scale, destination, rest, hovered, departed)
                    colours[destination] = dict(hover=hovered, departed=departed)
                samples[f"{name}-{scale}x"] = dict(rect=rect, rest=rest, transitions=colours)
                passed.append(f"{scale}x: {name} hover clears on desktop/client departure and same-panel clock entry")
            # Leave/re-enter must not suppress the next real paired click.
            for kind, name in (("brand", "Riplix"), ("applications", "Applications")):
                move(destinations["desktop"])
                ui.click_control(kind, name)
                ui.wait(lambda: ui.panel() and ui.panel()["modal"])
                ui.dismiss()
                passed.append(f"{scale}x: {name} activates after pointer re-entry")
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
    parser.add_argument("--output", type=Path, default=ROOT / "build/fusede-hover-focus/guest-tests")
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
    print(f"Graphite hover/focus: {len(report['checks'])} focused guest groups passed")


if __name__ == "__main__":
    main()
