#!/usr/bin/env python3
"""Real, offline Fuse-X window acceptance with a disposable 128 MiB root."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import time
import zlib

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "ports/tls/tests"))
from guest import Guest


def ppm(path: Path):
    try:
        raw = path.read_bytes()
    except FileNotFoundError:
        return None
    header = re.match(rb"P6\n(\d+) (\d+)\n255\n", raw)
    if not header:
        return None
    width, height = map(int, header.groups())
    return (width, height, raw[header.end():]) if len(raw) == header.end() + width * height * 3 else None


def region(frame, x, y, width, height):
    pitch = frame[0] * 3
    return b"".join(frame[2][(y + row) * pitch + x * 3:(y + row) * pitch + (x + width) * 3]
                    for row in range(height))


def png(frame):
    """Lossless screenshot packaging, without a host imaging dependency."""
    width, height, pixels = frame
    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body +
                struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff))
    rows = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


class Game:
    def __init__(self, binary: Path, data: Path, directory: Path):
        self.directory = directory
        self.frames = directory / "frames"
        self.mailbox = directory / "event.json"
        files = [dict(name="/dev/gfx", mode=0o20600, device=0x400),
                 dict(name="/dev/pcm", mode=0o20600, device=0x500),
                 dict(name="/data/home", directory=True, mode=0o40700)]
        files += [dict(name="/usr/share/lgeneral/" + path.relative_to(data).as_posix(),
                       path=str(path.resolve()), mode=0o100644)
                  for path in sorted(data.rglob("*")) if path.is_file()]
        old = dict(os.environ)
        os.environ.update(FUSE_X_FRAMES=str(self.frames), FUSE_X_TEST_EVENT=str(self.mailbox),
                          RCC_NATIVE_AUDIO="off")
        try:
            # C16 V3 physical filesystem blocks are 2048 octets.
            self.g = Guest(directory, "native", {"lgeneral": binary}, files,
                           blocks=65536, timeout=900)
        finally:
            os.environ.clear()
            os.environ.update(old)
        self.g.shell("HOME=/data/home; export HOME; LGENERAL_TRACE=1; export LGENERAL_TRACE")

    def wait(self, check, seconds=120, message="GUI condition"):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.g.pump()
            result = check()
            if result:
                return result
            if self.g.process.poll() is not None:
                break
        raise AssertionError(f"Timed out waiting for {message}:\n" + self.g.log[-6000:].decode(errors="replace"))

    def pause(self, seconds=.4):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.g.pump()

    def windows(self):
        try:
            return json.loads((self.frames / "windows.json").read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            return []

    def window(self):
        return next((w for w in self.windows() if w.get("visible") and "lgeneral" in w.get("title", "").lower()), None)

    def event(self, **fields):
        window = self.wait(self.window, message="LGeneral window")
        self.wait(lambda: not self.mailbox.exists(), message="input mailbox ready")
        temporary = self.mailbox.with_suffix(".tmp")
        temporary.write_text(json.dumps(dict(window=window["id"], **fields)))
        temporary.replace(self.mailbox)
        self.wait(lambda: not self.mailbox.exists() or not self.window(), message="input consumption")

    def key(self, key):
        self.event(input=dict(type="key", key=key, down=True))
        self.event(input=dict(type="key", key=key, down=False))

    def click(self, x, y, button=0):
        self.event(input=dict(type="motion", x=x, y=y))
        self.event(input=dict(type="button", button=button, down=True, x=x, y=y))
        self.event(input=dict(type="button", button=button, down=False, x=x, y=y))

    def frame(self):
        window = self.window()
        return ppm(self.frames / f"{window['id']}.ppm") if window else None

    def screenshot(self, output: Path, name: str):
        self.pause()
        frame = self.wait(self.frame, message=f"complete {name} frame")
        output.joinpath(name + ".ppm").write_bytes(
            f"P6\n{frame[0]} {frame[1]}\n255\n".encode() + frame[2])
        output.joinpath(name + ".png").write_bytes(png(frame))
        return frame

    def changed(self, before, rectangle=None):
        previous = region(before, *rectangle) if rectangle else before[2]
        def different():
            frame = self.frame()
            if not frame:
                return None
            current = region(frame, *rectangle) if rectangle else frame[2]
            return frame if current != previous else None
        return self.wait(different, message="changed application pixels")

    def menu(self, row):
        window = self.wait(self.window)
        self.click(window["width"] - 22, window["height"] - 258 + row * 36 + 18)

    def next_player_screen(self):
        frame = self.frame()
        if not frame or frame[:2] != (800, 600):
            return None
        # Original next-player interstitial: centred text on a black screen.
        # Loading progress also uses black, but starts at the upper left.
        centre = region(frame, 250, 240, 300, 120)
        outside = frame[2].count(0) - centre.count(0)
        outside_size = len(frame[2]) - len(centre)
        return frame if any(centre) and outside_size - outside < 30 else None

    def sidebar_ready(self, width, height):
        frame = self.frame()
        if not frame or frame[:2] != (width, height):
            return None
        patch = region(frame, width - 40, 8, 30, 30)
        pixels = zip(patch[::3], patch[1::3], patch[2::3])
        grayscale = sum(max(pixel) - min(pixel) <= 1 for pixel in pixels)
        return frame if any(patch) and grayscale > 800 else None


def run(binary: Path, data: Path, directory: Path) -> dict:
    """Run the real interactive executable; retain screenshots, logs and report."""
    binary, data, directory = map(lambda p: Path(p).resolve(), (binary, data, directory))
    if not binary.is_file():
        raise FileNotFoundError(f"Build the interactive LGeneral executable first: {binary}")
    if not (data / "assets-manifest.json").is_file():
        raise FileNotFoundError(f"Prepare the complete runtime assets first: {data}")
    directory.mkdir(parents=True, exist_ok=True)
    report = dict(executable_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), checks=[], screenshots=[])
    with tempfile.TemporaryDirectory(prefix="lgeneral-gui-", dir=directory) as temporary:
        game = Game(binary, data, Path(temporary))
        try:
            started = time.monotonic()
            game.g.shell("lgeneral &")
            game.g.shell("game=$!")
            game.wait(lambda: b"LGENERAL READY title" in game.g.log,
                      message="original title engine initialized")
            report["title_load_seconds"] = round(time.monotonic() - started, 3)
            window = game.wait(game.window, message="initial LGeneral window")
            game.wait(lambda: (frame := game.frame()) and len(set(frame[2])) > 64,
                      message="rendered original title and menu")
            assert (window["width"], window["height"]) == (800, 600), window
            title = game.screenshot(directory, "title")
            report["checks"].append("original title, bitmap fonts and menu")

            # Original sidebar: Load Campaign is the fourth 36-point icon.
            game.menu(3)
            game.changed(title)
            campaign = game.screenshot(directory, "campaign-selector")
            # Campaign selector = 120-point list + 220-point description pane.
            campaign_x, campaign_y = (800 - 44 - 340) // 2, (600 - 240) // 2
            game.click(campaign_x + 35, campaign_y + 20)
            game.changed(campaign, (campaign_x + 120, campaign_y, 220, 190))
            game.screenshot(directory, "campaign-selected")
            # Cancel leaves the title session intact.
            game.click(campaign_x + 119 + 220 - 30 + 10, campaign_y + 240 - 10 - 20 + 10)
            report["checks"].append("mouse campaign selection and metadata")

            game.menu(2)
            root_selector = game.screenshot(directory, "scenario-root")
            scenario_x, scenario_y = (800 - 44 - 360) // 2, (600 - 300) // 2
            game.click(scenario_x + 35, scenario_y + 20)  # kukgen directory
            game.changed(root_selector, (scenario_x, scenario_y, 120, 260))
            scenarios = game.screenshot(directory, "scenario-list")
            game.event(input=dict(type="motion", x=scenario_x + 55, y=scenario_y + 70))
            game.event(input=dict(type="wheel", x=0, y=-65536))
            game.changed(scenarios, (scenario_x + 10, scenario_y + 10, 100, 240))
            game.screenshot(directory, "scenario-scroll")
            game.event(input=dict(type="wheel", x=0, y=65536))
            game.pause()
            # First item is '..'; second is Bosnia in the packaged .order list.
            game.click(scenario_x + 35, scenario_y + 10 + 21 + 10)
            selected = game.screenshot(directory, "scenario-selected")
            scenario_started = time.monotonic()
            game.click(scenario_x + 119 + 240 - 60 + 10,
                       scenario_y + 220 - 10 - 20 + 10)
            game.wait(lambda: b"LGENERAL READY scenario" in game.g.log or game.next_player_screen(),
                      message="scenario map or original next-player interstitial")
            if b"LGENERAL READY scenario" not in game.g.log:
                game.screenshot(directory, "next-player")
                game.key(44)
            game.wait(lambda: b"LGENERAL READY scenario" in game.g.log,
                      message="selected scenario engine initialized")
            report["scenario_load_seconds"] = round(time.monotonic() - scenario_started, 3)
            game.changed(selected)
            game.pause(2)
            battle = game.screenshot(directory, "battlefield")
            assert region(battle, 0, 100, 700, 400) != region(title, 0, 100, 700, 400)
            report["checks"].append("scenario folder, wheel scrolling and playable map")

            # Tab mirrors the asymmetric UI even during the deployment phase;
            # combat shortcuts such as G intentionally require the normal turn.
            interaction_started = time.monotonic()
            game.key(43)  # HID Tab
            game.changed(battle)
            report["tab_mirror_seconds"] = round(time.monotonic() - interaction_started, 3)
            game.screenshot(directory, "battlefield-mirrored")
            report["checks"].append("keyboard panel control during deployment")
            report["resize_seconds"] = []
            for width, height in ((1024, 768), (800, 600)):
                resize_started = time.monotonic()
                game.event(resize=[width, height])
                game.wait(lambda: (w := game.window()) and (w["width"], w["height"]) == (width, height),
                          message=f"window resize {width}x{height}")
                game.wait(lambda: (f := game.frame()) and f[:2] == (width, height),
                          message=f"repaint after resize {width}x{height}")
                # The kernel initially publishes the previous surface at its
                # old size, surrounded by black. Wait for the game's sidebar
                # to be drawn at the new right edge, not just a new PPM header.
                game.wait(lambda: game.sidebar_ready(width, height),
                          message=f"application layout after resize {width}x{height}")
                report["resize_seconds"].append(dict(width=width, height=height,
                    seconds=round(time.monotonic() - resize_started, 3)))
                game.screenshot(directory, f"battlefield-{width}x{height}")
            report["checks"].append("live window resize")
            game.event(input=dict(type="close"))
            game.wait(lambda: not game.window(), message="window close and resource release")
            output = game.g.shell("wait $game; echo LGENERAL_GUI_EXIT:$?")
            assert "LGENERAL_GUI_EXIT:0" in output, output
            for bad in (b"panic:", b"GsError", b"runtime error:", b"NXR_BUFFER_FAILED", b"assertion failed"):
                assert bad not in game.g.log, game.g.log[-6000:]
            report["checks"].append("clean close, successful exit and filesystem check")
            report["status"] = "passed"
        except BaseException as error:
            report["status"] = "failed"
            report["error"] = str(error)
            frame = game.frame()
            if frame:
                (directory / "failure.png").write_bytes(png(frame))
            (directory / "result.json").write_text(json.dumps(report, indent=2) + "\n")
            raise
        finally:
            (directory / "guest.log").write_bytes(game.g.log)
            (directory / "windows.json").write_text(json.dumps(game.windows(), indent=2) + "\n")
            game.g.close()
    report["screenshots"] = [p.name for p in sorted(directory.glob("*.png"))]
    (directory / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/lgeneral/lgeneral.fxe")
    parser.add_argument("--data", type=Path, default=ROOT / "build/lgeneral/data")
    parser.add_argument("--output", type=Path, default=ROOT / "build/lgeneral/gui-test")
    args = parser.parse_args()
    print(json.dumps(run(args.binary, args.data, args.output), indent=2))
