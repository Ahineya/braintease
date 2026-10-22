#!/usr/bin/env python3
"""Exercise real GX apps inside the optional nested desktop on disposable disks."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("graphite_gui_test", ROOT / "ports/lvgl/gui-test.py")
gui_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gui_test)
GUI, Guest, ppm = gui_test.GUI, gui_test.Guest, gui_test.ppm


def save_png(path, image):
    """Save the captured RGB pixels without an optional image dependency."""
    width, height, pixels = image
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))
    rows = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
                     chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


class DesktopGUI(GUI):
    def __init__(self, output, kernel, build, calculator, desktop):
        self.out = Path(output)
        self.out.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="graphite-desktop-")
        work = Path(self.temp.name)
        self.frames, self.mailbox = work / "frames", work / "event.json"
        devices = [dict(name=f"/dev/{name}{i:x}", mode=0o20600, device=base + i)
                   for i in range(16) for name, base in (("ptyp", 0x300), ("ttyp", 0x202))]
        files = [dict(name="/dev/gfx", mode=0o20600, device=0x400), *devices]
        apps = {"lvcalc": calculator, "gterm": build / "apps/gterm.fxe",
                "kill": build / "apps/kill.fxe"}
        saved = dict(os.environ)
        os.environ.update(FUSE_X_TEST_KERNEL=str(kernel), FUSE_X_DESKTOP="1" if desktop else "0",
                          FUSE_X_DESKTOP_SIZE="1280x800", FUSE_X_FRAMES=str(self.frames),
                          FUSE_X_TEST_EVENT=str(self.mailbox), FUSE_X_TERM_STYLE="classic")
        try:
            self.g = Guest(work, "native", apps, files, blocks=65536, build_override=build)
        except BaseException:
            self.temp.cleanup()
            raise
        finally:
            os.environ.clear()
            os.environ.update(saved)
        self.g.shell("unset LVGL_SCALE")

    def desktop(self):
        try:
            return json.loads((self.frames / "desktop.json").read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            return {}

    def internal(self, wid):
        return next((w for w in self.desktop().get("windows", []) if w["id"] == wid), None)

    def scene(self):
        try:
            value = ppm(self.frames / "desktop.ppm")
            state = self.desktop()
            return value if value and value[:2] == (state.get("pixel_width"), state.get("pixel_height")) else None
        except FileNotFoundError:
            return None

    def save_scene(self, name):
        scene = self.wait(self.scene)
        shutil.copyfile(self.frames / "desktop.ppm", self.out / f"{name}.ppm")
        save_png(self.out / f"{name}.png", scene)
        (self.out / f"{name}.json").write_text(json.dumps(self.desktop(), indent=2) + "\n")

    def source(self, wid, width=300, height=360, scale=1):
        return self.frame(wid, scale=scale, width=width, height=height)

    def move(self, wid, x, y):
        before = self.wait(lambda: self.internal(wid))
        drag=before["chrome"]["drag"]
        px,py=drag[0]+min(12,drag[2]//2),drag[1]+drag[3]//2
        tx,ty=x+px-before["x"],y+py-before["y"]
        self.send(0, input=dict(type="button", button=0, down=True, x=px, y=py))
        self.send(0, input=dict(type="motion", x=tx, y=ty))
        self.send(0, input=dict(type="button", button=0, down=False, x=tx, y=ty))
        self.wait(lambda: self.internal(wid)["x"] == x and self.internal(wid)["y"] == y)

    def focus(self, wid):
        w = self.wait(lambda: self.internal(wid))
        self.click(0, w["x"] + 5, w["y"] + 5)
        self.wait(lambda: self.desktop().get("focused") == wid)

    def local_click(self, wid, x, y):
        w = self.wait(lambda: self.internal(wid))
        self.click(0, w["x"] + x, w["y"] + y)

    def control(self,wid,kind):
        w=self.wait(lambda:self.internal(wid))
        r=w["chrome"][kind]
        self.click(0,r[0]+r[2]//2,r[1]+r[3]//2)

    def restore(self,wid):
        w=self.wait(lambda:self.internal(wid));r=w["taskbar"]
        self.click(0,r[0]+r[2]//2,r[1]+r[3]//2)
        # Multiple instances share one dock icon; choose this exact window.
        menu=self.desktop().get("dock",{}).get("menu")
        if menu:
            entry=next(e for e in menu["entries"] if e["window"]==wid and e["command"]=="activate")
            r=entry["rect"]
            self.click(0,r[0]+r[2]//2,r[1]+r[3]//2)
        self.wait(lambda:not self.internal(wid)["minimized"] and self.desktop()["focused"]==wid)

    def maximized_source(self,wid):
        self.wait(lambda:self.internal(wid)["maximized"])
        bounds=self.desktop()["maximize_bounds"]
        self.source(wid,bounds[2],bounds[3],self.desktop()["scale"])
        self.wait(lambda:[self.internal(wid)[k] for k in ("x","y","width","height")]==self.desktop()["maximize_bounds"])
        return bounds

    def calculate(self, expression, expected):
        self.text(0, "c" + expression)
        at = len(self.g.log)
        self.key(0, 40)
        self.logged("CALC_RESULT " + expected, at)

    def source_matches_desktop(self, wid, width=300, height=360, scale=1):
        source = self.source(wid, width, height, scale)
        self.pause(.2)
        w = self.internal(wid)
        scene = self.wait(self.scene)
        desktop_scale = self.desktop()["scale"]
        assert desktop_scale == scale
        # The corner grip deliberately occupies fourteen logical client pixels.
        # Compare every other client pixel, including text and flat colour.
        rows = (height - (14 if w["chrome"]["grip"][2] else 0)) * scale
        for y in range(rows):
            at = ((w["y"] * scale + y) * scene[0] + w["x"] * scale) * 3
            expected = source[2][y * width * scale * 3:(y + 1) * width * scale * 3]
            actual = scene[2][at:at + width * scale * 3]
            assert actual == expected, f"Nested/source pixel mismatch in client {wid}, physical row {y}"
        return width * scale * rows

    def close(self):
        # Preserve diagnostic frames even if a check failed.
        if self.frames.exists():
            saved = self.out / "last-frames"
            saved.mkdir(exist_ok=True)
            for path in self.frames.iterdir():
                if path.suffix in (".ppm", ".json"):
                    shutil.copyfile(path, saved / path.name)
        super().close()


def native_reference(args):
    ui = DesktopGUI(args.output / "native", args.kernel, args.build, args.calculator, False)
    try:
        ui.g.shell("LVGL_SCALE=1 lvcalc --trace &")
        ui.wait(lambda: ui.window("Calculator"))
        wid = ui.window("Calculator")["id"]
        frame = ui.source(wid)
        (ui.out / "calculator.ppm").write_bytes((ui.frames / f"{wid}.ppm").read_bytes())
        assert not (ui.frames / "desktop.json").exists(), "Desktop enabled without opt-in"
        ui.text(wid, "c6*7")
        at = len(ui.g.log)
        ui.key(wid, 40)
        ui.logged("CALC_RESULT 42", at)
        ui.send(wid, close=True)
        ui.wait(lambda: not ui.windows())
        return frame
    finally:
        ui.close()


def nested_checks(args, reference):
    ui = DesktopGUI(args.output / "nested", args.kernel, args.build, args.calculator, True)
    checks, compared = [], 0
    try:
        ui.g.shell("GTERM_TRACE_INPUT=1 gterm -g 60x18 &")
        ui.g.shell("terminal=$!")
        ui.wait(lambda: ui.window("Terminal"))
        terminal = ui.window("Terminal")["id"]
        ui.g.shell("lvcalc --trace &")
        ui.g.shell("first=$!")
        ui.wait(lambda: ui.window("Calculator"))
        first = ui.window("Calculator")["id"]
        actual = ui.source(first)
        assert actual == reference, "Native and nested calculator published frames differ"
        compared += ui.source_matches_desktop(first)
        checks += ["unchanged native mode", "native/nested published-frame pixel parity", "nested client pixel parity"]
        ui.calculate("6*7", "42")
        # Hit stock calculator widgets through desktop coordinate translation.
        for x, y in ((43, 144), (43, 193), (256, 291), (185, 291)):
            ui.local_click(first, x, y)
        at = len(ui.g.log)
        ui.local_click(first, 256, 340)
        ui.logged("CALC_RESULT 10", at)
        checks += ["focused keyboard arithmetic", "translated stock-widget mouse arithmetic"]

        # Clear widget hover and finish its pending refresh before measuring
        # movement; a legitimate hover-state change is an application repaint.
        ui.send(0, input=dict(type="motion", x=1200, y=40))
        ui.source(first)
        old = dict(ui.internal(first))
        sequence = next(w["sequence"] for w in ui.windows() if w["id"] == first)
        ui.move(first, 800, 100)
        ui.wait(lambda: ui.internal(first)["x"] != old["x"])
        assert next(w["sequence"] for w in ui.windows() if w["id"] == first) == sequence, "Moving forced client repaint"
        compared += ui.source_matches_desktop(first)
        ui.focus(terminal)
        ui.text(0, "echo DESKTOP_PTY_OK > /tmp/desktop-marker")
        ui.key(0, 40)
        ui.wait(lambda: "DESKTOP_PTY_OK" in ui.g.shell("cat /tmp/desktop-marker"))
        checks += ["retained movement without client repaint", "terminal focus and shell/PTY text routing"]

        ui.g.shell("lvcalc --trace &")
        ui.g.shell("second=$!")
        ui.wait(lambda: len([w for w in ui.windows() if w["title"] == "Calculator"]) == 2)
        second = next(w["id"] for w in ui.windows() if w["title"] == "Calculator" and w["id"] != first)
        ui.move(second, 450, 100)
        ui.calculate("7+8", "15")
        ui.pause(.2)
        before = (ui.frames / f"{first}.ppm").read_bytes()
        ui.calculate("4*4", "16")
        assert (ui.frames / f"{first}.ppm").read_bytes() == before, "Input reached the other calculator process"
        checks.append("independent calculator processes and focused input isolation")

        ui.text(0, "c")
        w = ui.internal(second)
        at = len(ui.g.log)
        ui.send(0, input=dict(type="button", button=0, down=True, x=w["x"] + 43, y=w["y"] + 193))
        ui.send(0, input=dict(type="motion", x=10, y=10))
        ui.send(0, input=dict(type="button", button=0, down=False, x=10, y=10))
        ui.pause(.2)
        # Stock LVGL buttons use press_lock: their original button receives
        # the outside release. It must arrive exactly once, without leaving
        # the desktop or client in a stuck pressed state.
        assert bytes(ui.g.log[at:]).count(b"CALC_INPUT 7") == 1, "Captured release did not reach original owner exactly once"
        ui.calculate("9+1", "10")
        checks.append("captured outside release reaches original owner once and clears pressed state")

        w=ui.internal(second)
        c=w["chrome"]
        assert c["close"][0]<c["minimize"][0]<c["maximize"][0]<c["drag"][0]
        ui.control(second,"minimize")
        ui.wait(lambda: ui.internal(second)["minimized"])
        assert ui.desktop()["focused"] != second
        ui.restore(second)
        checks += ["minimize transfers focus", "grouped dock restores the selected retained window"]

        w = ui.internal(second)
        x, y = w["x"] + w["width"] - 5, w["y"] + w["height"] - 5
        ui.send(0, input=dict(type="button", button=0, down=True, x=x, y=y))
        ui.send(0, input=dict(type="motion", x=x + 80, y=y + 70))
        ui.send(0, input=dict(type="button", button=0, down=False, x=x + 80, y=y + 70))
        ui.source(second, 380, 430)
        ui.wait(lambda: (ui.internal(second)["width"], ui.internal(second)["height"]) == (380, 430))
        ui.calculate("12/4", "3")
        compared += ui.source_matches_desktop(second, 380, 430)
        checks.append("corner resize reaches real guest and preserves usable widgets")

        normal=[ui.internal(second)[k] for k in ("x","y","width","height")]
        ui.control(second,"maximize")
        bounds=ui.maximized_source(second)
        compared+=ui.source_matches_desktop(second,bounds[2],bounds[3])
        ui.send(0,resize=[1470,900])
        bounds=ui.maximized_source(second)
        ui.control(second,"minimize")
        ui.wait(lambda:ui.internal(second)["minimized"])
        assert ui.internal(second)["maximized"]
        ui.restore(second)
        assert ui.internal(second)["maximized"]
        ui.control(second,"maximize")
        ui.source(second,380,430)
        ui.wait(lambda:not ui.internal(second)["maximized"])
        assert [ui.internal(second)[k] for k in ("x","y","width","height")]==normal
        ui.control(second,"maximize")
        ui.maximized_source(second)
        drag=ui.internal(second)["chrome"]["drag"]
        px,py=drag[0]+min(12,drag[2]//2),drag[1]+drag[3]//2
        ui.send(0,input=dict(type="button",button=0,down=True,x=px,y=py))
        ui.wait(lambda:not ui.internal(second)["maximized"])
        ui.send(0,input=dict(type="motion",x=px+70,y=py+50))
        ui.send(0,input=dict(type="button",button=0,down=False,x=px+70,y=py+50))
        ui.source(second,380,430)
        ui.move(second,450,100)
        checks += ["shared left close/minimize/maximize geometry", "maximize uses available workspace", "maximized guest follows workspace resize", "minimize preserves maximized state", "restore preserves prior normal bounds", "title drag restores maximized window under pointer"]

        ui.send(0, density=2)
        ui.source(first, scale=2)
        ui.source(second, 380, 430, 2)
        ui.wait(lambda: ui.desktop().get("scale") == 2)
        compared += ui.source_matches_desktop(second, 380, 430, 2)
        for size in ((1470, 900), (1920, 1080), (1280, 800)):
            ui.send(0, resize=list(size))
            ui.wait(lambda: ui.scene() and ui.scene()[:2] == (size[0] * 2, size[1] * 2))
        ui.send(0, resize=[1470, 900])
        ui.wait(lambda: ui.scene() and ui.scene()[:2] == (2940, 1800))
        ui.save_scene("desktop-retina")
        ui.send(0, density=1)
        ui.source(first)
        ui.source(second, 380, 430)
        ui.wait(lambda: ui.desktop().get("scale") == 1)
        compared += ui.source_matches_desktop(second, 380, 430)
        checks += ["automatic 1x/2x/1x backing transitions", "desktop resize through full HD", "Retina client pixel parity"]

        ui.pause(.5)
        before = ui.desktop()["presents"]
        stamp = (ui.frames / "desktop.ppm").stat().st_mtime_ns
        ui.pause(1.3)
        assert ui.desktop()["presents"] == before, "Idle desktop keeps composing"
        assert (ui.frames / "desktop.ppm").stat().st_mtime_ns == stamp, "Idle desktop keeps publishing"
        checks.append("idle desktop stops composition and publication")
        ui.save_scene("desktop-1x")

        ui.control(second,"close")
        ui.wait(lambda: ui.internal(second) is None)
        assert "SECOND_STATUS:0" in ui.g.shell("wait $second; echo SECOND_STATUS:$?")
        assert ui.internal(first) and ui.internal(terminal)
        ui.g.shell("kill $first")
        ui.wait(lambda: ui.internal(first) is None)
        ui.g.shell("wait $first")
        ui.focus(terminal)
        ui.text(0, "echo STILL_ROUTED > /tmp/still-routed")
        ui.key(0, 40)
        ui.wait(lambda: "STILL_ROUTED" in ui.g.shell("cat /tmp/still-routed"))
        ui.control(terminal,"close")
        ui.wait(lambda: ui.desktop().get("windows") == [])
        ui.g.shell("wait $terminal")
        assert "SESSION_SURVIVED" in ui.g.shell("echo SESSION_SURVIVED")
        assert ui.desktop()["focused"] is None
        ui.save_scene("desktop-empty")
        checks += ["titlebar close and clean process wait", "forced owner termination removes window", "terminal survives other owner teardown", "last-window close leaves session running"]
        assert not any(error in ui.g.log for error in (b"native runtime error", b"panic:", b"graphics operation failed"))
        return dict(status="passed", checks=checks, compared_client_pixels=compared,
                    private_images_only=True, networking="off", native_windows_in_desktop=1,
                    presentation="headless shared desktop compositor; native host presentation is separate")
    finally:
        ui.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=ROOT / "build/lvgl/desktop-editor/fuse-x-desktop")
    parser.add_argument("--build", type=Path, default=ROOT / "fuse-x/build")
    parser.add_argument("--calculator", type=Path, default=ROOT / "build/lvgl/desktop-editor/apps/lvcalc.fxe")
    parser.add_argument("--output", type=Path, default=ROOT / "build/lvgl/desktop-editor/desktop-tests")
    args = parser.parse_args()
    for name in ("kernel", "build", "calculator", "output"):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    result = args.output / "result.json"
    result.unlink(missing_ok=True)
    started = time.monotonic()
    try:
        reference = native_reference(args)
        report = nested_checks(args, reference)
        report.update(kernel_sha256=hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
                      test_source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                      calculator_sha256=hashlib.sha256(args.calculator.read_bytes()).hexdigest(),
                      terminal_sha256=hashlib.sha256((args.build / "apps/gterm.fxe").read_bytes()).hexdigest(),
                      elapsed_seconds=round(time.monotonic() - started, 3))
    except BaseException as error:
        result.write_text(json.dumps(dict(status="failed", error=str(error)), indent=2) + "\n")
        raise
    result.write_text(json.dumps(report, indent=2) + "\n")
    print("Nested desktop: real terminal/calculator processes, input, retained windows, density, pixels, idle and teardown passed")


if __name__ == "__main__":
    main()
