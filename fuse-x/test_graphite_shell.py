#!/usr/bin/env python3
"""Exercise the actual Graphite shell and catalog on one private offline disk.

Uses production guest applications, panel policy, window registry, fonts, input
and compositor. The small Commander fixture deliberately creates an app/helper
process tree; it stands in for the interactive TUI, not for terminal launching.
Host native GPU presentation remains separate from this headless acceptance.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("desktop_shell_test", ROOT / "fuse-x/test_desktop_mode.py")
desktop_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(desktop_test)
CALCULATOR = "org.riplix.Calculator"
COMMANDER = "org.riplix.Commander"
TERMINAL = "org.riplix.Terminal"
BACKDROP = "org.riplix.PanelBackdrop"
KEYBOARD_APP = "org.riplix.Keyboard%02d"


def pgm(path):
    data = Path(path).read_bytes()
    match = re.match(rb"P5\n(\d+) (\d+)\n255\n", data)
    if not match:
        return None
    width, height = map(int, match.groups())
    pixels = data[match.end():]
    return (width, height, pixels) if len(pixels) == width * height else None


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def contains(rect, x, y):
    rx, ry, width, height = rect
    return rx <= x < rx + width and ry <= y < ry + height


class ShellGUI(desktop_test.DesktopGUI):
    def __init__(self, args, autostart=False, nested=True, keyboard_catalog=False):
        self.out = args.output / ("autostart" if nested else "native-autostart") if autostart else args.output
        if keyboard_catalog:
            self.out = args.output / "keyboard-navigation"
        self.out.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="graphite-shell-")
        work = Path(self.temp.name)
        self.frames, self.mailbox = work / "frames", work / "event.json"
        self.shell_at = 0
        self.shell_window = None
        self.run_terminal = False
        devices = [dict(name=f"/dev/{name}{i:x}", mode=0o20600, device=base + i)
                   for i in range(16) for name, base in (("ptyp", 0x300), ("ttyp", 0x202))]
        files = [dict(name="/dev/gfx", mode=0o20666, device=0x400), *devices,
                 dict(name="/root", directory=True, mode=0o40755),
                 dict(name="/bin/mc", mode=0o100755, path=str(args.fixture)),
                 dict(name="/bin/panel-backdrop", mode=0o100755, path=str(args.fixture)),
                 dict(name="/bin/run-no-exec", mode=0o100644, path=str(args.fixture)),
                 dict(name="/bin/run-bad-format", mode=0o100755, text="not an executable image\n")]
        files += [dict(name="/usr/share/applications/" + path.name, mode=0o100644, path=str(path))
                  for path in sorted(args.catalog.glob("*.ini"))]
        if keyboard_catalog:
            # Real catalog rows make the viewport overflow. Unavailable rows
            # before, between and after eligible entries must never be targets.
            for number in range(1, 21):
                files.append(dict(name=f"/usr/share/applications/keyboard-{number:02}.ini",
                                  mode=0o100644,
                                  text=("[application]\n"
                                        f"id={KEYBOARD_APP % number}\n"
                                        f"name=Navigation {number:02}\n"
                                        "icon=calculator\ncategory=Keyboard tests\n"
                                        "keywords=keyboard-fixture\nkind=gui\n"
                                        "exec=/bin/lvcalc\npinned=false\n")))
            files.append(dict(name="/usr/share/applications/keyboard-unavailable.ini", mode=0o100644,
                              text=("[application]\nid=org.riplix.KeyboardUnavailable\n"
                                    "name=Navigation 01 unavailable\nicon=calculator\n"
                                    "category=Keyboard tests\nkeywords=keyboard-fixture\n"
                                    "kind=gui\nexec=/bin/unavailable-keyboard-program\n"
                                    "pinned=false\n")))
            for edge, number in (("First", "00"), ("Last", "99")):
                files.append(dict(name=f"/usr/share/applications/keyboard-unavailable-{number}.ini", mode=0o100644,
                                  text=(f"[application]\nid=org.riplix.KeyboardUnavailable{edge}\n"
                                        f"name=Navigation {number} unavailable\nicon=calculator\n"
                                        "category=Keyboard tests\nkeywords=keyboard-fixture\n"
                                        "kind=gui\nexec=/bin/unavailable-keyboard-program\n"
                                        "pinned=false\n")))
        applications = dict(fusedeshell=args.shell, lvcalc=args.calculator,
                            gterm=args.terminal, kill=args.build / "apps/kill.fxe")
        kernel = args.kernel
        if autostart:
            kernel = work / "launch-with-gui"
            kernel.write_text("#!/bin/sh\nexec " + shlex.quote(str(args.kernel)) + ' "$@" --gui\n')
            kernel.chmod(0o755)
        saved = dict(os.environ)
        os.environ.update(FUSE_X_TEST_KERNEL=str(kernel), FUSE_X_DESKTOP="1" if nested else "0",
                          FUSE_X_DESKTOP_SIZE="1280x800", FUSE_X_FRAMES=str(self.frames),
                          FUSE_X_TEST_EVENT=str(self.mailbox), FUSE_X_TERM_STYLE="classic",
                          RCC_NATIVE_AUDIO="off")
        try:
            self.g = desktop_test.Guest(work, "native", applications, files,
                                        blocks=131072, build_override=args.build, timeout=600)
        except BaseException:
            self.temp.cleanup()
            raise
        finally:
            os.environ.clear()
            os.environ.update(saved)
        self.g.shell("HOME=/root; export HOME; unset LVGL_SCALE")

    def start_shell(self):
        self.shell_at = len(self.g.log)
        self.run_terminal = False
        self.g.shell("fusedeshell --trace &")
        self.logged("FUSEDE_READY", self.shell_at)
        self.shell_window = self.wait(lambda: self.window("FuseDE"))["id"]
        self.wait(lambda: (self.desktop().get("panel") or {}).get("window") == self.shell_window)
        self.wait(lambda: len(self.items()) >= 3)
        self.pause(.2)
        return self.shell_window

    def panel(self):
        return self.desktop().get("panel")

    def menu_painted(self):
        panel = self.panel()
        if not panel or not panel["modal"] or len(panel["regions"]) < 4:
            return False
        try:
            source = desktop_test.ppm(self.frames / (str(self.shell_window) + ".ppm"))
        except FileNotFoundError:
            return False
        if not source:
            return False
        alpha = None
        if panel.get("alpha"):
            try:
                data = (self.frames / (str(self.shell_window) + ".alpha.pgm")).read_bytes()
            except FileNotFoundError:
                return False
            match = re.match(rb"P5\n(\d+) (\d+)\n255\n", data)
            if not match:
                return False
            alpha = data[match.end():]
            if len(alpha) != source[0] * source[1]:
                return False
        scale = self.desktop()["scale"]
        x, y, width, height = panel["regions"][-1]
        # Closed shell backing pixels contain only the desktop background here.
        # Examine the upper menu half: this excludes the dock even at 600 high.
        different = 0
        for py in range((y + 8) * scale, (y + height // 2) * scale, max(1, 3 * scale)):
            for px in range((x + 8) * scale, (x + width - 8) * scale, max(1, 3 * scale)):
                at = (py * source[0] + px) * 3
                if (alpha[py * source[0] + px] >= 128 if alpha is not None else
                        source[2][at:at + 3] != bytes((0x15, 0x19, 0x1c))):
                    different += 1
        return different > 100

    def save_scene(self, name):
        # CONFIGURE/trace can precede LVGL's flush. Wait for source publication,
        # then for the compositor to consume that exact immutable frame.
        self.pause(.3)
        if self.panel() and self.panel().get("alpha"):
            # A density acknowledgement clears the resized panel backing before
            # LVGL publishes its first frame. Metadata can already say scale=2
            # while the last PGM/PPM still contains the old 1x frame.
            self.wait(self.alpha_source)
        if self.panel() and self.panel()["modal"]:
            self.wait(self.menu_painted)
        if self.panel():
            def published():
                surface = next((w for w in self.windows() if w["id"] == self.shell_window), None)
                return surface and self.panel() and self.panel()["sequence"] == surface["sequence"]
            self.wait(published)
        super().save_scene(name)
        alpha = self.frames / (str(self.shell_window) + ".alpha.pgm")
        if alpha.exists():
            shutil.copyfile(alpha, self.out / (name + ".alpha.pgm"))
            shutil.copyfile(self.frames / (str(self.shell_window) + ".ppm"),
                            self.out / (name + ".shell.ppm"))

    def controls(self, at=None):
        values = {}
        text = bytes(self.g.log[self.shell_at if at is None else at:]).decode(errors="replace")
        for kind, name, x, y, width, height in re.findall(
                r"FUSEDE_CONTROL\|([^|\r\n]+)\|([^|\r\n]+)\|(-?\d+)\|(-?\d+)\|(\d+)\|(\d+)", text):
            values[(kind, name)] = list(map(int, (x, y, width, height)))
        return values

    def items(self):
        text = bytes(self.g.log[self.shell_at:]).decode(errors="replace")
        if "FUSEDE_LAYOUT " not in text:
            return {}
        text = text.rsplit("FUSEDE_LAYOUT ", 1)[1]
        result = {}
        for app, x, y, width, height, windows, pinned in re.findall(
                r"FUSEDE_ITEM (\S+) (-?\d+),(-?\d+),(\d+),(\d+) windows=(\d+) pinned=(\d+)", text):
            result[app] = dict(rect=list(map(int, (x, y, width, height))),
                               windows=int(windows), pinned=bool(int(pinned)))
        return result

    def click_rect(self, rect, button=0):
        x, y, width, height = rect
        assert width > 0 and height > 0, rect
        self.send(0, input=dict(type="button", button=button, down=True,
                               x=x + width // 2, y=y + height // 2))
        self.send(0, input=dict(type="button", button=button, down=False,
                               x=x + width // 2, y=y + height // 2))

    def click_control(self, kind, name, button=0, at=None):
        rect = self.wait(lambda: self.controls(at).get((kind, name)))
        panel = self.wait(self.panel)
        assert any(contains(r, rect[0] + rect[2] // 2, rect[1] + rect[3] // 2)
                   for r in panel["regions"]), (kind, name, rect, panel)
        self.click_rect(rect, button)

    def app_click(self, app, button=0):
        item = self.wait(lambda: self.items().get(app))
        self.click_rect(item["rect"], button)

    def open_applications(self):
        at = len(self.g.log)
        self.click_control("applications", "Applications")
        self.logged("FUSEDE_APPLICATIONS", at)
        self.wait(lambda: self.panel() and self.panel()["modal"])
        return at

    def open_run(self):
        at = self.open_applications()
        self.click_control("action", "Run…", at=at)
        self.logged("FUSEDE_RUN_DIALOG", at)
        self.wait(self.menu_painted)
        region = self.panel()["regions"][-1]
        assert abs(region[0] * 2 + region[2] - self.desktop()["width"]) <= 2, region
        assert abs(region[1] * 2 + region[3] - self.desktop()["height"]) <= 2, region
        return at

    def run_command(self, command, terminal=False, submit=True):
        at = self.open_run()
        self.click_control("run-command", "Command", at=at)
        self.send(0, action=4)  # GX_ACTION_SELECT_ALL, independent of keybindings.
        self.text(0, command)
        if self.run_terminal != terminal:
            self.click_control("run-terminal", "Run in terminal", at=at)
            self.run_terminal = terminal
        self.pause(.1)
        if submit:
            self.click_control("action", "Run", at=at)
        return at

    def compact_run(self, at, error=False):
        self.pause(.2)
        self.wait(self.menu_painted)
        if error:
            self.wait(lambda: self.controls(at).get(("run-status", "Status"), [0, 0, 0, 0])[3] > 0)
        controls = self.controls(at)
        menu = self.panel()["regions"][-1]
        field = controls[("run-command", "Command")]
        terminal = controls[("run-terminal", "Run in terminal")]
        run = controls[("action", "Run")]
        cancel = controls[("action", "Cancel")]
        title = controls[("run-title", "Run")]
        assert menu[3] <= (240 if error else 210), ("oversized Run dialog", menu)
        assert abs(menu[0] * 2 + menu[2] - self.desktop()["width"]) <= 2, menu
        assert abs(menu[1] * 2 + menu[3] - self.desktop()["height"]) <= 2, menu
        for name, rect in (("title", title), ("command", field), ("terminal", terminal), ("Run", run), ("Cancel", cancel)):
            assert (rect[0] >= menu[0] + 8 and rect[1] >= menu[1] + 8 and
                    rect[0] + rect[2] <= menu[0] + menu[2] - 8 and
                    rect[1] + rect[3] <= menu[1] + menu[3] - 8), (name, rect, menu)
        assert field[1] - menu[1] <= 64, ("excess content above command", field, menu)
        assert 0 <= terminal[1] - field[1] - field[3] <= 12, (field, terminal)
        assert abs(run[1] - cancel[1]) <= 1, (run, cancel)
        assert 8 <= menu[1] + menu[3] - run[1] - run[3] <= 16, ("empty space below buttons", run, menu)
        source, _ = self.wait(self.alpha_source)
        scale = self.desktop()["scale"]
        def pixel(x, y):
            index = ((y * scale + scale // 2) * source[0] + x * scale + scale // 2) * 3
            return source[2][index:index + 3]
        # Bounds traces precede drawing. Prove the field is really painted at
        # those coordinates, catching focus-induced scrolling after layout.
        panel_colour = pixel(menu[0] + 4, menu[1] + menu[3] // 2)
        field_colour = pixel(field[0] + field[2] - 4, field[1] + field[3] // 2)
        border_colour = pixel(field[0] + field[2] // 2, field[1])
        assert field_colour != panel_colour and border_colour not in (panel_colour, field_colour), (
            "command field is not visibly painted at its rectangle", field, field_colour, border_colour, panel_colour)
        def visible_ink(rect):
            return any(max(pixel(x, y)) > 100 for y in range(rect[1], rect[1] + rect[3])
                       for x in range(rect[0], rect[0] + rect[2]))
        assert visible_ink(title), ("Run title is clipped or missing", title)
        status = controls.get(("run-status", "Status"))
        if error:
            assert status and status[3] >= 16, ("clipped inline error", status)
            assert terminal[1] + terminal[3] <= status[1] and status[1] + status[3] <= run[1], (terminal, status, run)
            assert status[0] >= menu[0] + 8 and status[0] + status[2] <= menu[0] + menu[2] - 8, (status, menu)
            assert visible_ink(status), ("inline error is not visibly painted", status)
        else:
            assert status is None, ("empty error row is visible", status)
            assert run[1] - terminal[1] - terminal[3] <= 16, ("empty status row", terminal, run, status)
        return dict(menu=menu, title=title, command=field, terminal=terminal, status=status, run=run, cancel=cancel)

    def dismiss(self):
        self.key(0, 41)
        self.wait(lambda: self.panel() and not self.panel()["modal"])

    def search(self, value):
        at = len(self.g.log)
        self.text(0, value)
        # Mailbox consumption acknowledges host delivery, not guest processing.
        # A partial query can contain the target at its old row coordinates;
        # wait for the final filter's frame/trace to settle before using them.
        self.pause(.5)
        last, stable = None, time.monotonic()
        end = time.monotonic() + 60
        while time.monotonic() < end:
            self.g.pump()
            surface = next((w for w in self.windows() if w["id"] == self.shell_window), None)
            state = (surface and surface["sequence"], tuple(sorted((k, tuple(v)) for k, v in self.controls(at).items())))
            if state != last:
                last, stable = state, time.monotonic()
            if time.monotonic() - stable >= .25:
                return at
        raise AssertionError("Application search never settled")

    def replace_search(self, value):
        self.click_control("search", "search")
        self.send(0, action=4)
        return self.search(value)

    def selections(self, at=None):
        text = bytes(self.g.log[self.shell_at if at is None else at:]).decode(errors="replace")
        matches = re.findall(r"FUSEDE_SELECTION\|([^|\r\n]*)\|(-?\d+)\|(\d+)\|(-?\d+)\|(-?\d+)\|(\d+)\|(-?\d+)\|(\d+)\|([^\r\n]*)", text)
        return [dict(app=app, index=int(index), count=int(count), scroll=int(scroll),
                     row_y=int(row_y), row_height=int(row_height), viewport_y=int(viewport_y),
                     viewport_height=int(viewport_height), query=query)
                for app, index, count, scroll, row_y, row_height, viewport_y, viewport_height, query in matches]

    def selection(self, at=None):
        values = self.selections(at)
        return values[-1] if values else None

    def selected(self, app, query, at=None):
        def expected():
            selection = self.selection(at)
            return selection if selection and selection["app"] == app and selection["query"] == query else None
        return self.wait(expected)

    def selected_visible(self, app, query, at=None):
        selection = self.selected(app, query, at)
        assert selection["row_height"] > 0 and selection["viewport_y"] <= selection["row_y"], selection
        assert selection["row_y"] + selection["row_height"] <= selection["viewport_y"] + selection["viewport_height"], selection
        return selection

    def application_row_color(self, app):
        row = self.controls()[("application", app)]
        source, _ = self.wait(self.alpha_source)
        scale = self.desktop()["scale"]
        # Sample the free right side of the short fixture names, away from
        # icons/text and rounded corners; this is the rendered row background.
        x = (row[0] + row[2] - 10) * scale + scale // 2
        y = (row[1] + row[3] // 2) * scale + scale // 2
        index = (y * source[0] + x) * 3
        return tuple(source[2][index:index + 3])

    def context(self, app):
        at = len(self.g.log)
        self.app_click(app, 1)
        self.logged("FUSEDE_WINDOWS app=" + app, at)
        self.wait(lambda: self.panel() and self.panel()["modal"])
        return at

    def choose(self, name, at=None):
        self.click_control("action", name, at=at)
        self.wait(lambda: self.panel() and not self.panel()["modal"])

    def calculators(self):
        return [w for w in self.windows() if w["title"] == "Calculator"]

    def painted_calculators(self, count):
        windows = self.wait(lambda: self.calculators() if len(self.calculators()) == count else None)
        for window in windows:
            def painted():
                try:
                    source = desktop_test.ppm(self.frames / (str(window["id"]) + ".ppm"))
                except FileNotFoundError:
                    return False
                return source and any(source[2])
            self.wait(painted)
        self.pause(.2)
        return windows

    def exit_shell(self):
        at = len(self.g.log)
        self.click_control("brand", "Riplix")
        self.click_control("action", "Exit desktop shell", at=at)
        self.wait(lambda: self.panel() is None)
        self.wait(lambda: not any(w["id"] == self.shell_window for w in self.windows()))
        self.pause(.1)

    def alpha_source(self):
        state = self.desktop()
        try:
            alpha = pgm(self.frames / (str(self.shell_window) + ".alpha.pgm"))
            source = desktop_test.ppm(self.frames / (str(self.shell_window) + ".ppm"))
        except FileNotFoundError:
            return None
        dimensions = state.get("pixel_width"), state.get("pixel_height")
        return (source, alpha) if source and alpha and source[:2] == alpha[:2] == dimensions else None

    def alpha_over_white(self, region, backdrop):
        self.pause(.2)
        self.wait(lambda: self.panel()["sequence"] == next(w["sequence"] for w in self.windows() if w["id"] == self.shell_window))
        source, alpha = self.wait(self.alpha_source)
        scene = self.wait(self.scene)
        scale = self.desktop()["scale"]
        white = self.internal(backdrop)
        counts = dict(zero=0, partial=0, opaque=0)
        x, y, width, height = region
        for py in range(y * scale, (y + height) * scale):
            for px in range(x * scale, (x + width) * scale):
                if not contains((white["x"] * scale, white["y"] * scale,
                                 white["width"] * scale, white["height"] * scale), px, py):
                    continue
                index = py * source[0] + px
                coverage = alpha[2][index]
                if coverage == 0:
                    counts["zero"] += 1
                elif coverage == 255:
                    counts["opaque"] += 1
                else:
                    counts["partial"] += 1
                actual = scene[2][index * 3:index * 3 + 3]
                expected = bytes((channel * coverage + 255 * (255 - coverage) + 127) // 255
                                 for channel in source[2][index * 3:index * 3 + 3])
                assert actual == expected, (region, px, py, coverage, actual, expected, white)
        assert counts["zero"] > 0 and counts["partial"] > 0 and counts["opaque"] > 0, (region, counts)
        return counts

    def transparent_corner(self, region):
        _, alpha = self.wait(self.alpha_source)
        scale = self.desktop()["scale"]
        x, y, width, height = region
        for dy in range(min(16, height)):
            for dx in range(min(16, width)):
                px, py = (x + dx) * scale + scale // 2, (y + dy) * scale + scale // 2
                if alpha[2][py * alpha[0] + px] == 0:
                    return x + dx, y + dy
        raise AssertionError(("No transparent rounded corner", region))

    def close(self):
        if self.frames.exists():
            saved = self.out / "last-frames"
            saved.mkdir(exist_ok=True)
            for path in self.frames.glob("*.alpha.pgm"):
                shutil.copyfile(path, saved / path.name)
        super().close()


def run_dialog_checks(ui, passed):
    at = ui.open_run()
    for control in (("run-command", "Command"), ("run-terminal", "Run in terminal"),
                    ("action", "Run"), ("action", "Cancel")):
        assert control in ui.controls(at), (control, ui.controls(at))
    geometry = dict(normal=ui.compact_run(at))
    ui.save_scene("centered-run-dialog")
    ui.choose("Cancel", at)
    passed.append("Applications Run dialog is centered and exposes command, terminal, Run and Cancel controls")

    for command in ("lvcalc", "/bin/lvcalc"):
        at = ui.run_command(command)
        ui.logged("FUSEDE_RUN|" + command, at)
        ui.wait(lambda: ui.panel() and not ui.panel()["modal"])
    calculators = ui.painted_calculators(2)
    ui.wait(lambda: ui.items().get(CALCULATOR, {}).get("windows") == 2)
    assert len([key for key in ui.items() if key == CALCULATOR]) == 1
    at = ui.context(CALCULATOR)
    ui.choose("Close all windows", at)
    ui.wait(lambda: not ui.calculators())
    passed.append("Run resolves program name and explicit path to two real calculator windows in one app group")

    command = "/bin/panel-backdrop --record /tmp/run-arguments 'two words' '$HOME; `false`; $(false)'"
    at = ui.run_command(command, terminal=True)
    terminal = ui.wait(lambda: ui.window("panel-backdrop"))["id"]
    ui.wait(lambda: ui.panel() and not ui.panel()["modal"])
    ui.wait(lambda: any(key.startswith("org.riplix.Run.") and value["windows"] == 1 for key, value in ui.items().items()))
    # A gterm exec acknowledgement precedes its PTY child's exec/file write.
    # Wait for the child's explicit proof rather than using window creation as
    # a proxy for completed child startup.
    def arguments_written():
        recorded = ui.g.shell("cat /tmp/run-arguments")
        return recorded if "ARG4=" in recorded else None
    recorded = ui.wait(arguments_written)
    assert "ARG3=<two words>" in recorded, recorded
    assert "ARG4=<$HOME; `false`; $(false)>" in recorded, recorded
    assert "WHITE_READY" not in bytes(ui.g.log[at:]).decode(errors="replace")
    closed_at = len(ui.g.log)
    ui.control(terminal, "close")
    ui.wait(lambda: not any(w["id"] == terminal for w in ui.windows()))
    ui.logged("FUSEDE_DOCK apps=4 windows=1", closed_at)
    ui.wait(lambda: ("applications", "Applications") in ui.controls(closed_at))
    ui.wait(lambda: len(ui.items()) == 4 and not any(key.startswith("org.riplix.Run.") for key in ui.items()))
    ui.pause(.15)
    passed.append("Run in terminal preserves quoted and literal shell-like arguments without evaluating or adding helper windows")

    for command in ("unavailable-program-graphite", "/bin/run-no-exec", "lvcalc 'unfinished"):
        before = {w["id"] for w in ui.windows()}
        at = ui.run_command(command)
        ui.logged("FUSEDE_RUN_ERROR|", at)
        assert ui.panel()["modal"]
        assert before == {w["id"] for w in ui.windows()}
        assert not re.search(rb"FUSEDE_RUN\|[^\r\n]+\|pid=\d+", bytes(ui.g.log[at:]))
        ui.dismiss()
    at = ui.run_command("/bin/run-bad-format")
    ui.logged("FUSEDE_RUN_ERROR|Cannot start program:", at)
    assert ui.panel()["modal"]
    geometry["error"] = ui.compact_run(at, error=True)
    ui.save_scene("run-exec-error-retains-dialog")
    ui.dismiss()
    passed.append("Run keeps unknown, non-executable, malformed and asynchronous exec failures in the modal without launching another program")

    for escape in (True, False):
        before = {w["id"] for w in ui.windows()}
        at = ui.run_command("/bin/panel-backdrop --record /tmp/run-canceled", submit=False)
        if escape:
            ui.dismiss()
        else:
            ui.choose("Cancel", at)
        ui.pause(.1)
        assert before == {w["id"] for w in ui.windows()}
        assert b"FUSEDE_RUN|" not in ui.g.log[at:]
    recorded = ui.g.shell("cat /tmp/run-canceled")
    assert "ARG0=" not in recorded, recorded
    passed.append("Escape and Cancel dismiss Run without executing the retained command")
    return geometry


def alpha_panel_checks(ui, backdrop, name, passed):
    assert ui.panel().get("alpha"), ui.panel()
    ui.focus(backdrop)
    ui.save_scene(name + "-white-dock")
    dock = ui.panel()["regions"][2]
    dock_counts = ui.alpha_over_white(dock, backdrop)
    corner = ui.transparent_corner(dock)
    at = len(ui.g.log)
    ui.click(0, *corner)
    ui.logged("WHITE_BUTTON down=1", at)
    ui.logged("WHITE_BUTTON down=0", at)
    assert ui.desktop()["focused"] == backdrop

    # Hover painting may change the tile, but its rounded corners retain the
    # parent dock pixels. Record both the visual and a concrete clipped sample.
    ui.send(0, input=dict(type="motion", x=20, y=120))
    ui.pause(.2)
    before, _ = ui.wait(ui.alpha_source)
    launcher = ui.controls()[("applications", "Applications")]
    ui.send(0, input=dict(type="motion", x=launcher[0] + launcher[2] // 2,
                         y=launcher[1] + launcher[3] // 2))
    ui.pause(.2)
    after, _ = ui.wait(ui.alpha_source)
    scale = ui.desktop()["scale"]
    index = ((launcher[1] * scale + scale // 2) * after[0] + launcher[0] * scale + scale // 2) * 3
    assert after[2][index:index + 3] == before[2][index:index + 3]
    ui.save_scene(name + "-launcher-hover")

    ui.context(CALCULATOR)
    ui.save_scene(name + "-white-context")
    menu = ui.panel()["regions"][-1]
    assert abs(menu[0] * 2 + menu[2] - ui.desktop()["width"]) <= 2, menu
    context_counts = ui.alpha_over_white(menu, backdrop)
    corner = ui.transparent_corner(menu)
    at = len(ui.g.log)
    ui.click(0, *corner)
    ui.wait(lambda: ui.panel() and not ui.panel()["modal"])
    ui.pause(.2)
    assert b"WHITE_BUTTON" not in ui.g.log[at:]

    ui.open_applications()
    ui.save_scene(name + "-white-applications")
    menu = ui.panel()["regions"][-1]
    assert abs(menu[0] * 2 + menu[2] - ui.desktop()["width"]) <= 2, menu
    applications_counts = ui.alpha_over_white(menu, backdrop)
    corner = ui.transparent_corner(menu)
    at = len(ui.g.log)
    ui.click(0, *corner)
    ui.wait(lambda: ui.panel() and not ui.panel()["modal"])
    ui.pause(.2)
    assert b"WHITE_BUTTON" not in ui.g.log[at:]
    passed.append(name + ": dock/context/Applications zero and partial alpha match straight-RGBA over a real white client; dock corners pass clicks and modal corners consume dismissal")
    return dict(dock=dock_counts, context=context_counts, applications=applications_counts)


def held_navigation_checks(ui, passed):
    ui.open_applications()
    searched = ui.replace_search("navigation")
    ui.selected_visible(KEYBOARD_APP % 1, "navigation", searched)
    at = len(ui.g.log)
    try:
        # Exactly one physical press: every later move must come from the
        # production shared input repeat timer, not repeated test injections.
        ui.send(0, input=dict(type="key", key=81, down=True))
        ui.wait(lambda: len(ui.selections(at)) >= 4, seconds=3)
    finally:
        ui.send(0, input=dict(type="key", key=81, down=False))
    ui.pause(.2)  # Drain events already queued before release.
    moves = ui.selections(at)
    assert len(moves) >= 4
    for index, move in enumerate(moves):
        assert move["query"] == "navigation" and move["count"] == 20, move
        assert move["index"] == (index + 1) % 20, moves
    released = ui.selection()
    count = len(ui.selections(at))
    ui.pause(.75)
    assert len(ui.selections(at)) == count and ui.selection() == released, (moves, ui.selections(at))
    ui.save_scene("keyboard-held-down-released")

    initial_query = "navigationxxxxxx"
    searched = ui.replace_search(initial_query)
    ui.selected("", initial_query, searched)
    at = len(ui.g.log)
    try:
        # Backspace is handled by LVGL's keypad path rather than the shell's
        # raw arrow callback; this validates the common edit-key repeat path.
        ui.send(0, input=dict(type="key", key=42, down=True))
        ui.wait(lambda: len({s["query"] for s in ui.selections(at)}) >= 3, seconds=3)
    finally:
        ui.send(0, input=dict(type="key", key=42, down=False))
    ui.pause(.2)
    edits = ui.selections(at)
    previous = initial_query
    for edit in edits:
        assert edit["query"] == previous[:-1], (previous, edit, edits)
        previous = edit["query"]
    assert len(edits) >= 3, edits
    count = len(edits)
    ui.pause(.75)
    assert len(ui.selections(at)) == count and ui.selection()["query"] == previous
    ui.save_scene("keyboard-held-backspace-released")
    passed.append("single held Down and Backspace presses repeat through raw navigation and LVGL editing paths, then stop on release")

    searched = ui.replace_search("navigation")
    ui.selected_visible(KEYBOARD_APP % 1, "navigation", searched)
    at = len(ui.g.log)
    try:
        ui.send(0, input=dict(type="key", key=81, down=True))
        ui.wait(lambda: len(ui.selections(at)) >= 4, seconds=3)
        ui.key(0, 43)  # Tab away from the search field.
        ui.send(0, input=dict(type="key", key=225, down=True))
        ui.key(0, 43)  # Shift+Tab returns without releasing held Down.
        ui.send(0, input=dict(type="key", key=225, down=False))
        ui.pause(.2)
        focused = ui.selection()
        count = len(ui.selections(at))
        ui.pause(.75)
        assert len(ui.selections(at)) == count and ui.selection() == focused
    finally:
        ui.send(0, input=dict(type="key", key=81, down=False))
    # Prove focus really returned to search, rather than hiding continued
    # repeat delivery by leaving focus on a category or unrelated button.
    before = len(ui.g.log)
    ui.key(0, 81)
    expected = KEYBOARD_APP % ((focused["index"] + 1) % 20 + 1)
    ui.selected_visible(expected, "navigation", before)

    searched = ui.replace_search("navigation")
    ui.selected_visible(KEYBOARD_APP % 1, "navigation", searched)
    at = len(ui.g.log)
    try:
        ui.send(0, input=dict(type="key", key=81, down=True))
        ui.wait(lambda: len(ui.selections(at)) >= 4, seconds=3)
        ui.dismiss()
        # Keep Down physically held across keyboard-owner loss and a newly
        # opened modal; the old press must not start navigating that new menu.
        ui.open_applications()
        searched = ui.replace_search("navigation")
        ui.selected_visible(KEYBOARD_APP % 1, "navigation", searched)
        settled = len(ui.selections(searched))
        ui.pause(.75)
        assert ui.selection()["app"] == KEYBOARD_APP % 1
        assert len(ui.selections(searched)) == settled
    finally:
        ui.send(0, input=dict(type="key", key=81, down=False))
    ui.dismiss()
    passed.append("changing search focus or dismissing Applications cancels a held navigation key without resuming its old press")
    return dict(moves_from_one_press=len(moves), released_index=released["index"],
                backspace_edits_from_one_press=len(edits), backspace_stopped_after_release=True,
                navigation_stopped_after_release=True, navigation_stopped_after_widget_focus_change=True,
                navigation_stopped_after_modal_dismissal=True)


def keyboard_checks(args):
    ui = ShellGUI(args, keyboard_catalog=True)
    passed, scroll_positions = [], {}
    try:
        ui.start_shell()
        at = ui.open_applications()
        ui.click_control("search", "search", at=at)
        searched = ui.search("navigation")
        first = ui.selected_visible(KEYBOARD_APP % 1, "navigation", searched)
        assert first["count"] == 20 and first["index"] == 0, first
        assert ("application", "org.riplix.KeyboardUnavailable") in ui.controls(searched)
        assert ("application", "org.riplix.KeyboardUnavailableFirst") in ui.controls(searched)
        assert ("application", "org.riplix.KeyboardUnavailableLast") in ui.controls(searched)
        ui.save_scene("keyboard-first-selected")
        before_colors = [ui.application_row_color(KEYBOARD_APP % number) for number in (1, 2)]
        before = len(ui.g.log)
        ui.key(0, 81)  # Down: the visible unavailable row is skipped.
        second = ui.selected_visible(KEYBOARD_APP % 2, "navigation", before)
        assert second["index"] == 1, second
        ui.save_scene("keyboard-second-selected")
        after_colors = [ui.application_row_color(KEYBOARD_APP % number) for number in (1, 2)]
        assert all(old != new for old, new in zip(before_colors, after_colors)), (before_colors, after_colors)
        assert before_colors[0] == after_colors[1] and before_colors[1] == after_colors[0], (before_colors, after_colors)
        before = len(ui.g.log)
        ui.key(0, 82)
        ui.selected_visible(KEYBOARD_APP % 1, "navigation", before)
        before = len(ui.g.log)
        ui.key(0, 82)  # Up wraps across unavailable tail/head rows.
        wrapped = ui.selected_visible(KEYBOARD_APP % 20, "navigation", before)
        assert wrapped["index"] == 19, wrapped
        before = len(ui.g.log)
        ui.key(0, 81)  # Down wraps to the first available result.
        wrapped = ui.selected_visible(KEYBOARD_APP % 1, "navigation", before)
        assert wrapped["index"] == 0, wrapped
        passed.append("Applications Up/Down moves the visible selection and wraps in both directions while skipping unavailable rows")

        for number in range(2, 21):
            before = len(ui.g.log)
            ui.key(0, 81)
            selection = ui.selected_visible(KEYBOARD_APP % number, "navigation", before)
        assert selection["index"] == 19 and selection["scroll"] > 0, selection
        scroll_positions["normal-1x"] = selection
        ui.save_scene("keyboard-last-result-1x")
        before = len(ui.g.log)
        ui.key(0, 81)
        ui.selected_visible(KEYBOARD_APP % 1, "navigation", before)
        before = len(ui.g.log)
        ui.key(0, 82)
        ui.selected_visible(KEYBOARD_APP % 20, "navigation", before)
        before = len(ui.g.log)
        ui.key(0, 82)
        ui.selected_visible(KEYBOARD_APP % 19, "navigation", before)
        passed.append("keyboard selection scrolls each chosen application into the viewport, including both wraparound transitions")

        # No pointer refocus: text after arrow navigation must still edit the
        # search field, then reset selection to the first eligible new match.
        searched = ui.search(" 20")
        selection = ui.selected_visible(KEYBOARD_APP % 20, "navigation 20", searched)
        assert selection["count"] == 1 and selection["index"] == 0, selection
        for key in (81, 82, 81, 82):
            before = len(ui.g.log)
            ui.key(0, key)
            single = ui.selected_visible(KEYBOARD_APP % 20, "navigation 20", before)
            assert single["count"] == 1 and single["index"] == 0, single
        ui.save_scene("keyboard-filter-after-arrows")
        before = len(ui.g.log)
        ui.key(0, 40)
        ui.logged("FUSEDE_LAUNCH app=" + KEYBOARD_APP % 20 + " state=1", before)
        calculator = ui.painted_calculators(1)[0]["id"]
        ui.wait(lambda: ui.panel() and not ui.panel()["modal"])
        ui.control(calculator, "close")
        ui.wait(lambda: not ui.calculators())
        ui.pause(.15)
        passed.append("typing after arrows remains in application search, resets selection, and Enter launches that selected catalog app")

        ui.open_applications()
        searched = ui.replace_search("navigation")
        ui.selected_visible(KEYBOARD_APP % 1, "navigation", searched)
        before = len(ui.g.log)
        ui.key(0, 81)
        ui.selected_visible(KEYBOARD_APP % 2, "navigation", before)
        searched = ui.replace_search("no-such-navigation-result")
        empty = ui.selected("", "no-such-navigation-result", searched)
        assert empty["index"] == -1 and empty["count"] == 0, empty
        windows = {w["id"] for w in ui.windows()}
        before = len(ui.g.log)
        for key in (81, 82, 40):
            ui.key(0, key)
        ui.pause(.2)
        assert ui.panel()["modal"] and windows == {w["id"] for w in ui.windows()}
        assert not re.search(rb"FUSEDE_LAUNCH app=\S+ state=[12] ", bytes(ui.g.log[before:]))
        ui.save_scene("keyboard-no-matches")
        searched = ui.replace_search("unavailable")
        unavailable = ui.selected("", "unavailable", searched)
        assert unavailable["index"] == -1 and unavailable["count"] == 0, unavailable
        unavailable_ids = {"org.riplix.KeyboardUnavailable", "org.riplix.KeyboardUnavailableFirst",
                           "org.riplix.KeyboardUnavailableLast"}
        assert unavailable_ids <= {name for kind, name in ui.controls(searched) if kind == "application"}
        before = len(ui.g.log)
        for key in (81, 82, 40):
            ui.key(0, key)
        ui.pause(.2)
        assert ui.panel()["modal"] and windows == {w["id"] for w in ui.windows()}
        assert not re.search(rb"FUSEDE_LAUNCH app=\S+ state=[12] ", bytes(ui.g.log[before:]))
        ui.save_scene("keyboard-no-available-apps")
        searched = ui.replace_search("navigation")
        reset = ui.selected_visible(KEYBOARD_APP % 1, "navigation", searched)
        assert reset["index"] == 0 and reset["scroll"] == 0, reset
        before = len(ui.g.log)
        ui.key(0, 81)
        ui.selected_visible(KEYBOARD_APP % 2, "navigation", before)
        before = len(ui.g.log)
        ui.click_control("category", "Keyboard tests")
        reset = ui.selected_visible(KEYBOARD_APP % 1, "navigation", before)
        assert reset["index"] == 0, reset
        searched = ui.search(" 20")
        ui.selected_visible(KEYBOARD_APP % 20, "navigation 20", searched)
        ui.dismiss()
        passed.append("query/category changes reset selection; zero eligible results stay inert and a single eligible result stays selected")

        repeat = held_navigation_checks(ui, passed)

        ui.send(0, resize=[800, 600])
        ui.wait(lambda: ui.desktop().get("width") == 800 and ui.desktop().get("height") == 600)
        ui.send(0, density=2)
        ui.wait(lambda: ui.desktop().get("scale") == 2)
        ui.save_scene("keyboard-narrow-desktop-2x")
        at = ui.open_applications()
        ui.click_control("search", "search", at=at)
        searched = ui.search("navigation")
        ui.selected_visible(KEYBOARD_APP % 1, "navigation", searched)
        for number in range(2, 21):
            before = len(ui.g.log)
            ui.key(0, 81)
            selection = ui.selected_visible(KEYBOARD_APP % number, "navigation", before)
        assert selection["scroll"] > 0, selection
        scroll_positions["narrow-2x"] = selection
        ui.save_scene("keyboard-last-result-2x")
        before = len(ui.g.log)
        ui.key(0, 81)
        ui.selected_visible(KEYBOARD_APP % 1, "navigation", before)
        before = len(ui.g.log)
        ui.key(0, 82)
        ui.selected_visible(KEYBOARD_APP % 20, "navigation", before)
        searched = ui.search(" 20")
        ui.selected_visible(KEYBOARD_APP % 20, "navigation 20", searched)
        before = len(ui.g.log)
        ui.key(0, 40)
        ui.logged("FUSEDE_LAUNCH app=" + KEYBOARD_APP % 20 + " state=1", before)
        ui.painted_calculators(1)
        ui.wait(lambda: ui.panel() and not ui.panel()["modal"])
        passed.append("narrow 2x navigation scrolls and wraps eligible results, preserves typing focus, and launches with Enter")
        assert b"FUSEDE_ERROR" not in ui.g.log
        return dict(checks=passed, scroll_positions=scroll_positions, repeat=repeat)
    finally:
        ui.close()


def checks(args):
    ui = ShellGUI(args)
    passed = []
    alpha_results = {}
    run_layouts = {}
    try:
        shell = ui.start_shell()
        panel = ui.panel()
        assert ui.internal(shell) is None, ui.desktop()
        assert ui.desktop()["dock"]["visible"] is False
        assert len(panel["regions"]) == 3, panel
        brand, clock, dock = panel["regions"]
        assert brand[0] + brand[2] < clock[0] and brand[1] < 40 and clock[1] < 40
        assert dock[2] < ui.desktop()["width"] and dock[1] > ui.desktop()["height"] // 2
        assert panel["reserve_top"] == 40 and panel["reserve_bottom"] == 76
        clock_pattern = rb"FUSEDE_CLOCK\|(?:Sun|Mon|Tue|Wed|Thu|Fri|Sat) \d{2} (?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) \d{2}:\d{2}"
        ui.wait(lambda: re.search(clock_pattern, bytes(ui.g.log[ui.shell_at:])))
        assert ui.items()[CALCULATOR] == dict(rect=ui.items()[CALCULATOR]["rect"], windows=0, pinned=True)
        app_button = ui.controls()[("applications", "Applications")]
        assert app_button[0] < min(item["rect"][0] for item in ui.items().values())
        for item in ui.items().values():
            x, y, iw, ih = item["rect"]
            assert dock[0] <= x and dock[1] <= y and x + iw <= dock[0] + dock[2] and y + ih <= dock[1] + dock[3], (dock, item)
        passed += ["manager shell is a panel, excluded from ordinary windows and fallback dock",
                   "disconnected brand and formatted clock panels, bounded dock and work-area reservations",
                   "leftmost application button and idle pinned GUI/TUI launchers"]
        ui.save_scene("normal-pinned-desktop")

        at = ui.open_applications()
        ui.wait(lambda: ("category", "Accessories") in ui.controls(at))
        assert ("category", "Development") in ui.controls(at)
        assert ("search", "search") in ui.controls(at)
        assert ("application", COMMANDER) in ui.controls(at)
        ui.save_scene("classic-applications-menu")
        ui.click_control("category", "Accessories", at=at)
        ui.dismiss()
        passed.append("classic searchable application menu with explicit categories and GUI/TUI entries")

        ui.app_click(CALCULATOR)
        first = ui.painted_calculators(1)[0]["id"]
        ui.wait(lambda: ui.items().get(CALCULATOR, {}).get("windows") == 1)
        assert len([key for key in ui.items() if key == CALCULATOR]) == 1
        at = ui.context(CALCULATOR)
        ui.choose("Open new", at)
        ui.painted_calculators(2)
        ids = {w["id"] for w in ui.calculators()}
        ui.wait(lambda: ui.items().get(CALCULATOR, {}).get("windows") == 2)
        at = ui.context(CALCULATOR)
        ui.save_scene("grouped-calculator-menu")
        ui.choose("Minimize all windows", at)
        ui.wait(lambda: all(ui.internal(wid)["minimized"] for wid in ids))
        at = ui.context(CALCULATOR)
        ui.choose("Show all windows", at)
        ui.wait(lambda: all(not ui.internal(wid)["minimized"] for wid in ids))
        at = ui.context(CALCULATOR)
        ui.choose("Close all windows", at)
        ui.wait(lambda: not ui.calculators())
        ui.wait(lambda: ui.items().get(CALCULATOR, {}).get("windows") == 0)
        passed += ["pinned launch merges with application identity across independent instances",
                   "context menu opens new instance and minimizes, restores and cooperatively closes app windows",
                   "closing last window retains its inactive pinned launcher"]

        run_layouts["normal-1x"] = run_dialog_checks(ui, passed)

        at = ui.open_applications()
        searched = ui.search("Commander")
        ui.wait(lambda: ("application", COMMANDER) in ui.controls(searched))
        ui.save_scene("searched-commander")
        ui.click_control("application", COMMANDER, at=searched)
        commander = ui.wait(lambda: ui.window("Commander"))["id"]
        ui.wait(lambda: ui.items().get(COMMANDER, {}).get("windows") == 1)
        ui.wait(lambda: ui.panel() and not ui.panel()["modal"])
        ui.app_click(TERMINAL)
        terminal = ui.wait(lambda: ui.window("Terminal"))["id"]
        ui.wait(lambda: ui.items().get(TERMINAL, {}).get("windows") == 1)
        assert commander != terminal
        assert not any(key.startswith("window-process-") for key in ui.items())
        assert len(ui.windows()) == 3, ui.windows()
        passed += ["search launches Commander in gterm with its own stable app identity",
                   "terminal application child and helper processes add no dock entries or windows"]

        at = ui.context(CALCULATOR)
        ui.choose("Unpin from dock", at)
        ui.wait(lambda: CALCULATOR not in ui.items())
        saved = ui.g.shell("cat /root/.config/fusede/pins.ini")
        assert CALCULATOR + "=false" in saved.replace(" ", ""), saved
        ui.exit_shell()
        assert {commander, terminal} <= {w["id"] for w in ui.windows()}
        assert ui.desktop()["dock"]["visible"] is True
        assert ui.desktop()["maximize_bounds"][1] == 28
        ui.start_shell()
        ui.wait(lambda: CALCULATOR not in ui.items())
        passed += ["unpin persists atomically in the per-user INI and survives shell restart",
                   "graceful shell exit restores fallback dock and reservations without terminating apps"]

        at = ui.open_applications()
        searched = ui.search("Calculator")
        ui.wait(lambda: ("application", CALCULATOR) in ui.controls(searched))
        ui.click_control("application", CALCULATOR, button=1, at=searched)
        ui.logged("FUSEDE_WINDOWS app=" + CALCULATOR, searched)
        ui.choose("Pin to dock", searched)
        ui.wait(lambda: ui.items().get(CALCULATOR, {}).get("pinned"))
        saved = ui.g.shell("cat /root/.config/fusede/pins.ini")
        assert CALCULATOR + "=true" in saved.replace(" ", ""), saved
        ui.exit_shell()
        ui.start_shell()
        ui.wait(lambda: ui.items().get(CALCULATOR, {}).get("pinned"))
        assert ui.items()[CALCULATOR]["windows"] == 0
        passed.append("pinning from the application menu survives restart as an inactive launcher")

        ui.app_click(CALCULATOR)
        calculator = ui.painted_calculators(1)[0]["id"]
        ui.wait(lambda: ui.internal(calculator))
        ui.move(calculator, 24, 500)
        source = ui.source(calculator)
        ui.focus(commander)
        ui.pause(.2)
        scene = ui.wait(ui.scene)
        x, y = 50, 750
        assert not any(contains(r, x, y) for r in ui.panel()["regions"])
        c = ui.internal(calculator)
        offset = ((y - c["y"]) * source[0] + x - c["x"]) * 3
        expected = source[2][offset:offset + 3]
        actual = scene[2][(y * scene[0] + x) * 3:(y * scene[0] + x) * 3 + 3]
        assert actual == expected, (actual, expected, c)
        ui.click(0, x, y)
        ui.wait(lambda: ui.desktop()["focused"] == calculator)
        passed.append("client pixels and pointer input remain intact beside the floating dock")
        ui.save_scene("apps-and-disconnected-shell")

        at = ui.run_command("/bin/panel-backdrop --white")
        ui.logged("WHITE_READY", at)
        backdrop = ui.wait(lambda: ui.window("White backdrop"))["id"]
        ui.wait(lambda: ui.panel() and not ui.panel()["modal"])
        ui.wait(lambda: ui.items().get(BACKDROP, {}).get("windows") == 1)
        ui.send(backdrop, resize=[1100, 820])
        ui.logged("WHITE_RESIZE width=1100 height=820", at)
        ui.wait(lambda: ui.internal(backdrop)["width"] == 1100 and ui.internal(backdrop)["height"] == 820)
        ui.move(backdrop, 8, 76)
        alpha_results["normal-1x"] = alpha_panel_checks(ui, backdrop, "normal-1x", passed)

        ui.send(0, resize=[800, 600])
        ui.wait(lambda: ui.desktop().get("width") == 800 and ui.desktop().get("height") == 600)
        ui.wait(lambda: any(w["id"] == ui.shell_window and w["width"] == 800 and w["height"] == 600 for w in ui.windows()))
        ui.wait(lambda: ui.panel() and all(r[0] >= 0 and r[1] >= 0 and r[0] + r[2] <= 800 and r[1] + r[3] <= 600 for r in ui.panel()["regions"]))
        ui.save_scene("narrow-desktop")
        ui.send(0, density=2)
        ui.wait(lambda: ui.desktop().get("scale") == 2)
        ui.wait(lambda: next((w["scale"] for w in ui.windows() if w["id"] == ui.shell_window), 0) == 2)
        ui.save_scene("narrow-desktop-2x")
        at = ui.open_applications()
        ui.wait(lambda: ("search", "search") in ui.controls(at))
        ui.save_scene("narrow-applications-menu-2x")
        ui.dismiss()
        passed.append("output resize and 2x backing density preserve bounded panels and usable application menu")
        ui.send(backdrop, resize=[900, 700])
        ui.logged("WHITE_RESIZE width=900 height=700")
        ui.wait(lambda: ui.internal(backdrop)["width"] == 900 and ui.internal(backdrop)["height"] == 700)
        ui.move(backdrop, 8, 76)
        alpha_results["narrow-2x"] = alpha_panel_checks(ui, backdrop, "narrow-2x", passed)
        at = ui.open_run()
        ui.click_control("run-command", "Command", at=at)
        ui.send(0, action=4)
        ui.key(0, 42)
        run_layouts["narrow-2x"] = dict(normal=ui.compact_run(at))
        ui.save_scene("narrow-centered-run-dialog-2x")
        ui.choose("Cancel", at)
        passed.append("Run and application/context popups remain centered and launcher hover remains rounded at narrow 2x density")
        before = {w["id"] for w in ui.windows()}
        at = ui.run_command("/bin/run-bad-format")
        ui.logged("FUSEDE_RUN_ERROR|Cannot start program:", at)
        assert ui.panel()["modal"] and before == {w["id"] for w in ui.windows()}
        run_layouts["narrow-2x"]["error"] = ui.compact_run(at, error=True)
        ui.save_scene("narrow-compact-run-error-2x")
        ui.choose("Cancel", at)
        passed.append("Run stays compact at 1x and 2x with tightly spaced controls and fully contained inline errors")

        ui.send(0, resize=[1280, 800])
        ui.send(0, density=1)
        ui.wait(lambda: ui.desktop().get("width") == 1280 and ui.desktop().get("scale") == 1)
        ui.control(backdrop, "close")
        ui.wait(lambda: not any(w["id"] == backdrop for w in ui.windows()))
        ui.focus(commander)
        # Output reconfiguration completes asynchronously in every client. Let
        # all accepted resizes, density acknowledgements and registry snapshots
        # settle before measuring the steady idle contract.
        last, stable = None, time.monotonic()
        quiet_until = time.monotonic() + 30
        while time.monotonic() < quiet_until:
            ui.g.pump()
            desktop = ui.desktop()
            state = (desktop.get("presents"), desktop.get("composed_pixels"),
                     tuple((w["id"], w["sequence"], w["configuration"]) for w in ui.windows()))
            if state != last:
                last, stable = state, time.monotonic()
            if time.monotonic() - stable >= 1.2:
                break
        else:
            raise AssertionError("Output reconfiguration never became idle")
        for attempt in range(3):
            clock_before = re.findall(clock_pattern, bytes(ui.g.log))[-1]
            before = ui.desktop()
            sequence = next(w["sequence"] for w in ui.windows() if w["id"] == ui.shell_window)
            ui.pause(.8)
            if re.findall(clock_pattern, bytes(ui.g.log))[-1] != clock_before:
                continue
            after = ui.desktop()
            assert after["presents"] == before["presents"], (before["presents"], after["presents"])
            assert after["composed_pixels"] == before["composed_pixels"]
            assert next(w["sequence"] for w in ui.windows() if w["id"] == ui.shell_window) == sequence
            break
        else:
            raise AssertionError("Could not sample idle behavior within one clock minute")
        passed.append("clock updates at minute granularity and idle shell causes no repaints or composition")
        ui.save_scene("final-desktop")

        pid = next(w["pid"] for w in ui.windows() if w["id"] == ui.shell_window)
        surviving = {w["id"] for w in ui.windows() if w["id"] != ui.shell_window}
        ui.g.shell("kill -9 " + str(pid))
        ui.wait(lambda: ui.panel() is None and ui.desktop().get("dock", {}).get("visible"))
        assert surviving <= {w["id"] for w in ui.windows()}
        assert ui.desktop()["maximize_bounds"][1] == 28
        ui.save_scene("shell-crash-fallback")
        passed.append("forced shell death revokes overlay and restores fallback while independent apps survive")
        assert not any(marker in ui.g.log for marker in (b"panic:", b"native runtime error", b"graphics operation failed", b"FUSEDE_ERROR")), ui.g.log[-6000:].decode(errors="replace")
        return dict(status="passed", checks=passed, alpha_over_white=alpha_results, run_layouts=run_layouts,
                    private_images_only=True, networking="off",
                    presentation="production guest/policy/compositor headless; native GPU presentation separate",
                    commander_fixture="native app and forked helper blocked on a private pipe in a gterm PTY; --white renders a separate real GX client")
    finally:
        ui.close()


def autostart_checks(args):
    passed = []
    for nested in (True, False):
        ui = ShellGUI(args, autostart=True, nested=nested)
        try:
            if nested:
                shell = ui.wait(lambda: ui.window("FuseDE"))["id"]
                ui.shell_window = shell
                ui.wait(lambda: ui.panel() and ui.panel()["window"] == shell)
                assert ui.internal(shell) is None
                assert not ui.desktop()["dock"]["visible"]
                assert not ui.window("Terminal"), ui.windows()
                ui.save_scene("boot-graphite-shell")
                passed.append("nested --gui init starts the Graphite shell once with installed catalog")
            else:
                ui.wait(lambda: ui.window("Terminal"))
                ui.pause(.2)
                assert not ui.window("FuseDE"), ui.windows()
                assert b"this display does not support desktop panels" not in ui.g.log
                passed.append("native-window --gui retains graphical terminal startup without Graphite panel attempts")
        finally:
            ui.close()
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=ROOT / "build/fusede-shell/fuse-x")
    parser.add_argument("--shell", type=Path, default=ROOT / "build/graphite-shell/lvgl/fusedeshell.fxe")
    parser.add_argument("--calculator", type=Path, default=ROOT / "build/lvgl/dock-identity/lvcalc.fxe")
    parser.add_argument("--terminal", type=Path, default=ROOT / "build/fusede-shell/gterm.fxe")
    parser.add_argument("--fixture", type=Path, default=ROOT / "build/fusede-shell/graphite-tui.fxe")
    parser.add_argument("--catalog", type=Path, default=ROOT / "ports/fusede/catalog")
    parser.add_argument("--build", type=Path, default=ROOT / "fuse-x/build")
    parser.add_argument("--output", type=Path, default=ROOT / "build/fusede-shell/guest-tests")
    args = parser.parse_args()
    for name in vars(args):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    result = args.output / "result.json"
    try:
        report = checks(args)
        keyboard = keyboard_checks(args)
        report["checks"] += keyboard.pop("checks")
        report["keyboard_navigation"] = keyboard
        report["checks"] += autostart_checks(args)
        report.update(kernel_sha256=digest(args.kernel), shell_sha256=digest(args.shell),
                      calculator_sha256=digest(args.calculator), terminal_sha256=digest(args.terminal),
                      fixture_sha256=digest(args.fixture),
                      runner_sha256=digest(__file__), elapsed_seconds=round(time.monotonic() - started, 3),
                      catalog_sha256={p.name: digest(p) for p in sorted(args.catalog.glob("*.ini"))})
    except BaseException as error:
        result.write_text(json.dumps(dict(status="failed", error=str(error)), indent=2) + "\n")
        raise
    result.write_text(json.dumps(report, indent=2) + "\n")
    print("Graphite shell: " + str(len(report["checks"])) + " guest acceptance groups passed")


if __name__ == "__main__":
    main()
