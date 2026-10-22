#!/usr/bin/env python3
"""Exercise grouped dock policy with real GX guest processes on a private disk."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("desktop_mode_test", ROOT / "fuse-x/test_desktop_mode.py")
desktop_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(desktop_test)


class DockGUI(desktop_test.DesktopGUI):
    def __init__(self, args):
        self.out = args.output
        self.out.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="fusede-dock-")
        work = Path(self.temp.name)
        self.frames, self.mailbox = work / "frames", work / "event.json"
        self.controller_started = False
        saved = dict(os.environ)
        os.environ.update(FUSE_X_TEST_KERNEL=str(args.kernel), FUSE_X_DESKTOP="1",
                          FUSE_X_DESKTOP_SIZE="1280x800", FUSE_X_FRAMES=str(self.frames),
                          FUSE_X_TEST_EVENT=str(self.mailbox), RCC_NATIVE_AUDIO="off")
        try:
            self.g = desktop_test.Guest(work, "native", {"dock-probe": args.probe},
                [dict(name="/dev/gfx", mode=0o20666, device=0x400)],
                blocks=32768, build_override=args.build, timeout=300)
        except BaseException:
            self.temp.cleanup()
            raise
        finally:
            os.environ.clear()
            os.environ.update(saved)

    def start(self, tag):
        if not self.controller_started:
            os.write(self.g.master, b"dock-probe --controller\r")
            self.logged("DOCK_CONTROL_READY")
            self.controller_started = True
        at = len(self.g.log)
        os.write(self.g.master, ("start " + tag + "\r").encode())
        pattern = rb"DOCK_READY " + tag.encode() + rb" (\d+) (\d+)"
        result = self.wait(lambda: re.search(pattern, bytes(self.g.log[at:])) or
                           (b"DOCK_FAILED" in self.g.log[at:] and True))
        assert result is not True, self.g.log[at:].decode(errors="replace")
        return int(result[2])

    def command(self, tag, command):
        at = len(self.g.log)
        os.write(self.g.master, (tag + ":" + command + "\r").encode())
        self.logged("DOCK_COMMAND " + tag + " " + command, at)

    def close(self):
        try:
            if self.controller_started and self.g.process.poll() is None:
                os.write(self.g.master, b"quit\r")
                self.logged("DOCK_CONTROL_DONE", seconds=15)
                self.controller_started = False
        finally:
            super().close()

    def dock(self):
        return self.desktop().get("dock", {})

    def groups(self):
        return self.dock().get("groups", [])

    def group_for(self, wid):
        return next((group for group in self.groups() if wid in group["windows"]), None)

    def menu(self):
        # The overlay is dock-owned; tolerate a top-level metadata alias.
        return self.dock().get("menu", self.desktop().get("menu"))

    def click_rect(self, rect, button=0):
        x, y, width, height = rect
        assert width > 0 and height > 0, rect
        self.send(0, input=dict(type="button", button=button, down=True,
                               x=x + width // 2, y=y + height // 2))
        self.send(0, input=dict(type="button", button=button, down=False,
                               x=x + width // 2, y=y + height // 2))

    def group_click(self, wid, button=0):
        group = self.wait(lambda: self.group_for(wid))
        self.click_rect(group["rect"], button)

    def entry(self, command, wid=0):
        menu = self.menu()
        return next((entry for entry in (menu or {}).get("entries", [])
                     if entry["command"] == command and (not wid or entry["window"] == wid)), None)

    def choose(self, command, wid=0):
        entry = self.wait(lambda: self.entry(command, wid))
        assert not entry.get("disabled", False), entry
        self.click_rect(entry["rect"])
        self.wait(lambda: self.menu() is None)


def inside(inner, outer):
    x, y, width, height = inner
    ox, oy, ow, oh = outer
    return width > 0 and height > 0 and ox <= x and oy <= y and x + width <= ox + ow and y + height <= oy + oh


def checks(args):
    ui = DockGUI(args)
    passed = []
    try:
        alpha, beta, other, legacy = [ui.start(tag) for tag in ("alpha", "beta", "other", "legacy")]
        ui.wait(lambda: len(ui.groups()) == 3)
        probe = ui.group_for(alpha)
        assert probe and probe["app_id"] == "org.riplix.dock-probe", ui.dock()
        assert set(probe["windows"]) == {alpha, beta}, ui.dock()
        assert ui.group_for(other)["app_id"] == "org.riplix.dock-other", ui.dock()
        assert ui.group_for(other)["key"] != probe["key"]
        assert ui.group_for(legacy)["key"] not in (probe["key"], ui.group_for(other)["key"])
        assert len([window for window in ui.windows() if not window["visible"]]) == 1
        assert {wid for group in ui.groups() for wid in group["windows"]} == {alpha, beta, other, legacy}
        assert b"DOCK_HELPER alpha " in ui.g.log
        passed += ["same app groups independent instances", "same title does not group different apps",
                   "explicit non-window helper and hidden surface do not create dock items",
                   "unregistered execution has a separate fallback item"]
        original_order = [group["key"] for group in ui.groups()]

        ui.control(legacy, "minimize")
        ui.wait(lambda: ui.internal(legacy)["minimized"])
        ui.group_click(legacy)
        ui.wait(lambda: not ui.internal(legacy)["minimized"] and ui.desktop()["focused"] == legacy)
        assert ui.menu() is None
        passed.append("single-window icon activates and restores minimized window")

        # Crossing the dock while a client owns the drag must preserve that
        # client's release; the next dock press must still work normally.
        w, r = ui.internal(legacy), ui.group_for(alpha)["rect"]
        x, y = w["x"] + 20, w["y"] + 20
        tx, ty = r[0] + r[2] // 2, r[1] + r[3] // 2
        before_clicks = bytes(ui.g.log).count(b"DOCK_CLICK legacy ")
        ui.send(0, input=dict(type="button", button=0, down=True, x=x, y=y))
        ui.send(0, input=dict(type="motion", x=tx, y=ty))
        ui.send(0, input=dict(type="button", button=0, down=False, x=tx, y=ty))
        ui.wait(lambda: bytes(ui.g.log).count(b"DOCK_CLICK legacy ") == before_clicks + 1)
        assert ui.menu() is None
        passed.append("client capture crossing dock retains outside release")

        ui.group_click(alpha)
        ui.wait(ui.menu)
        entries = [entry["window"] for entry in ui.menu()["entries"] if entry["command"] == "activate"]
        assert set(entries) == {alpha, beta}, ui.menu()
        ui.choose("activate", beta)
        ui.wait(lambda: ui.desktop()["focused"] == beta)
        assert [group["key"] for group in ui.groups()] == original_order
        passed += ["multi-window icon opens chooser and activates exact window",
                   "focus changes preserve dock group order"]

        ui.group_click(alpha, 1)
        ui.wait(ui.menu)
        assert {"minimize_all", "restore_all", "close_all"} <= {entry["command"] for entry in ui.menu()["entries"]}
        ui.choose("minimize_all")
        ui.wait(lambda: all(ui.internal(wid)["minimized"] for wid in (alpha, beta)))
        assert set(ui.group_for(alpha)["windows"]) == {alpha, beta}
        ui.group_click(alpha, 1)
        ui.choose("restore_all")
        ui.wait(lambda: all(not ui.internal(wid)["minimized"] for wid in (alpha, beta)))
        passed.append("context actions minimize and restore all real app windows")

        ui.group_click(alpha, 1)
        ui.wait(ui.menu)
        before = next(window["sequence"] for window in ui.windows() if window["id"] == alpha)
        ui.command("alpha", "t")
        renamed = ui.wait(lambda: ui.entry("activate", alpha)
                          if (ui.entry("activate", alpha) or {}).get("title") != "Alpha" else None)
        assert renamed["title"] == "Alpha renamed Ω", renamed
        assert next(window["sequence"] for window in ui.windows() if window["id"] == alpha) == before
        passed.append("open menu updates Unicode title without client repaint")
        ui.command("beta", "x")
        ui.wait(lambda: ui.internal(beta) is None)
        ui.wait(lambda: ui.entry("activate", beta) is None)
        assert ui.group_for(alpha)["windows"] == [alpha]
        assert b"DOCK_COMMAND beta x" in ui.g.log
        passed.append("destroyed window disappears while its process remains alive")
        ui.key(0, 41)
        ui.wait(lambda: ui.menu() is None)
        ui.group_click(alpha)
        ui.wait(lambda: ui.desktop()["focused"] == alpha)
        assert ui.menu() is None

        # A hidden backing surface only joins the group when explicitly mapped.
        ui.command("alpha", "s")
        ui.wait(lambda: len(ui.group_for(alpha)["windows"]) == 2)
        hidden = next(wid for wid in ui.group_for(alpha)["windows"] if wid != alpha)
        assert ui.internal(hidden)["title"] == "Hidden surface"
        ui.group_click(alpha, 1)
        ui.wait(lambda: ui.entry("activate", hidden))
        ui.command("alpha", "d")
        ui.wait(lambda: ui.entry("activate", hidden) is None)
        ui.key(0, 41)
        ui.wait(lambda: ui.menu() is None)
        passed.append("mapping and destroying a hidden surface reconciles group and open menu")

        ui.group_click(legacy, 1)
        ui.wait(ui.menu)
        before_clicks = bytes(ui.g.log).count(b"DOCK_CLICK ")
        before_keys = bytes(ui.g.log).count(b"DOCK_KEY ")
        ui.key(0, 41)
        ui.wait(lambda: ui.menu() is None)
        ui.pause(.15)
        assert bytes(ui.g.log).count(b"DOCK_KEY ") == before_keys, "Escape leaked through menu"
        ui.group_click(legacy, 1)
        ui.wait(ui.menu)
        w = ui.internal(alpha)
        point = [w["x"] + 15, w["y"] + 15, 2, 2]
        assert not inside(point, ui.menu()["rect"])
        ui.click_rect(point)
        ui.wait(lambda: ui.menu() is None)
        ui.pause(.15)
        assert bytes(ui.g.log).count(b"DOCK_CLICK ") == before_clicks, "Dismiss click reached a client"
        ui.click_rect(point)
        ui.wait(lambda: bytes(ui.g.log).count(b"DOCK_CLICK ") == before_clicks + 1)
        passed.append("Escape and outside dismissal do not leak input or leave mouse capture stuck")

        ui.send(0, resize=[320, 240])
        ui.wait(lambda: ui.desktop().get("width") == 320)
        ui.send(0, density=2)
        ui.wait(lambda: ui.desktop().get("scale") == 2)
        ui.group_click(alpha, 1)
        ui.wait(ui.menu)
        assert inside(ui.dock()["bounds"], [0, 0, 320, 240]), ui.dock()
        assert inside(ui.menu()["rect"], [0, 0, 320, 240]), ui.menu()
        for entry in ui.menu()["entries"]:
            assert inside(entry["rect"], ui.menu()["rect"]), entry
        ui.save_scene("dock-narrow-2x")
        ui.key(0, 41)
        ui.wait(lambda: ui.menu() is None)
        ui.send(0, density=1)
        ui.send(0, resize=[1280, 800])
        ui.wait(lambda: ui.desktop().get("width") == 1280 and ui.desktop().get("scale") == 1)
        passed.append("dock and context menu fit narrow logical desktop at 2x backing density")

        ui.group_click(alpha, 1)
        ui.choose("close_all")
        ui.wait(lambda: ui.internal(alpha) is None)
        ui.wait(lambda: not any(group["app_id"] == "org.riplix.dock-probe" for group in ui.groups()))
        assert ui.internal(other) and ui.internal(legacy)
        passed.append("cooperative close-all removes app icon despite surviving helper and owner processes")
        ui.group_click(other, 1)
        ui.wait(ui.menu)
        ui.command("other", "x")
        ui.wait(lambda: ui.menu() is None and ui.group_for(other) is None)
        passed.append("removing final anchor window dismisses its stale menu")

        ui.send(0, input=dict(type="motion", x=1200, y=40))
        ui.pause(.5)
        state, stamp = ui.desktop(), (ui.frames / "desktop.ppm").stat().st_mtime_ns
        ui.pause(1.2)
        assert ui.desktop()["presents"] == state["presents"], "Idle dock keeps composing"
        assert (ui.frames / "desktop.ppm").stat().st_mtime_ns == stamp, "Idle dock keeps publishing"
        ui.save_scene("dock-final")
        passed.append("idle dock stops composition and publication")
        for tag in ("alpha", "beta", "other", "legacy"):
            ui.command(tag, "q")
        ui.wait(lambda: not ui.desktop().get("windows"))
        assert not ui.groups()
        assert not any(marker in ui.g.log for marker in
                       (b"DOCK_FAILED", b"panic:", b"native runtime error", b"graphics operation failed"))
        return dict(status="passed", checks=passed, private_images_only=True, networking="off",
                    presentation="headless production desktop compositor; native GPU presentation is separate")
    finally:
        ui.close()


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=ROOT / "build/fusede-dock/fuse-x")
    parser.add_argument("--probe", type=Path, default=ROOT / "build/fusede-dock/dock-probe.fxe")
    parser.add_argument("--build", type=Path, default=ROOT / "fuse-x/build")
    parser.add_argument("--output", type=Path, default=ROOT / "build/fusede-dock/dock-tests")
    args = parser.parse_args()
    for name in ("kernel", "probe", "build", "output"):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    receipt = args.output / "result.json"
    receipt.unlink(missing_ok=True)
    started = time.monotonic()
    try:
        report = checks(args)
        report.update(kernel_sha256=sha256(args.kernel), probe_sha256=sha256(args.probe),
                      probe_source_sha256=sha256(ROOT / "fuse-x/examples/dock.c"),
                      test_source_sha256=sha256(__file__), elapsed_seconds=round(time.monotonic() - started, 3))
    except BaseException as error:
        receipt.write_text(json.dumps(dict(status="failed", error=str(error),
                          elapsed_seconds=round(time.monotonic() - started, 3)), indent=2) + "\n")
        raise
    receipt.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
