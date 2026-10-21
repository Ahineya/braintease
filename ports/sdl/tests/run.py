#!/usr/bin/env python3
"""Compare upstream/guest SDL pixels and exercise GX in a disposable disk.

Builds are intentionally separate: --host must link the unadapted pinned SDL,
not the guest pixel implementation. No user disk image is opened or modified.
"""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / "build/sdl"
sys.path.insert(0, str(ROOT / "ports/tls/tests"))
from guest import Guest


def records(output):
    return [line for line in output.replace("\r", "").splitlines()
            if line.startswith(("PIX ", "ROW", "RECT ", "RWOPS "))]


def ppm(path):
    data = path.read_bytes()
    match = re.match(rb"P6\n(\d+) (\d+)\n255\n", data)
    if not match:
        return None
    width, height = map(int, match.groups())
    pixels = data[match.end():]
    return (width, height, pixels) if len(pixels) == width * height * 3 else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=Path, default=BUILD / "sdlcheck-host")
    parser.add_argument("--guest", type=Path, default=BUILD / "sdlcheck.fxe")
    parser.add_argument("--demo", type=Path, default=BUILD / "sdl-demo.fxe")
    parser.add_argument("--output", type=Path, default=BUILD / "tests")
    parser.add_argument("--host-only", action="store_true")
    parser.add_argument("--surface-only", action="store_true", help="skip events, GUI and profile checks")
    parser.add_argument("--skip-gui", action="store_true")
    parser.add_argument("--skip-profile", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "result.json").unlink(missing_ok=True)
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    with tempfile.TemporaryDirectory(prefix="sdl-host-") as host_work:
        host = subprocess.run([str(args.host.resolve()), "--surface-only"], cwd=host_work,
                              env=env, text=True, capture_output=True, check=True)
    (args.output / "host-surfaces.log").write_text(host.stdout + host.stderr)
    assert "SDL_ACCEPTANCE --surface-only passed" in host.stdout, host.stdout + host.stderr
    reference = records(host.stdout)
    assert len([line for line in reference if line.startswith("PIX ")]) >= 70, "incomplete reference"
    if not args.surface_only:
        host_events = subprocess.run([str(args.host.resolve()), "--events"],
                                    env=env, text=True, capture_output=True, check=True)
        (args.output / "host-events.log").write_text(host_events.stdout + host_events.stderr)
        assert "SDL_ACCEPTANCE --events passed" in host_events.stdout
    if args.host_only:
        print(f"SDL upstream reference: {len(reference)} pixel/rectangle records passed")
        return

    applications = {"sdl-check": args.guest}
    gui = not args.surface_only and not args.skip_gui
    if gui:
        applications["sdl-demo"] = args.demo
    with tempfile.TemporaryDirectory(prefix="sdl-guest-") as directory:
        work = Path(directory)
        frames, mailbox = work / "frames", work / "input.json"
        saved = dict(os.environ)
        os.environ.update(FUSE_X_FRAMES=str(frames), FUSE_X_TEST_EVENT=str(mailbox))
        try:
            guest = Guest(work, "native", applications,
                          [dict(name="/dev/gfx", mode=0o20600, device=0x400)], blocks=131072)
        finally:
            os.environ.clear()
            os.environ.update(saved)

        def wait(condition, seconds=60):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                guest.pump()
                result = condition()
                if result:
                    return result
                assert guest.process.poll() is None, guest.log[-6000:].decode(errors="replace")
            raise AssertionError("SDL wait timed out:\n" + guest.log[-6000:].decode(errors="replace"))

        def pause(seconds):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                guest.pump()

        def windows():
            try:
                return json.loads((frames / "windows.json").read_text())
            except (FileNotFoundError, json.JSONDecodeError):
                return []

        def window():
            return next((w for w in windows() if w["visible"] and w["title"] == "SDL software test"), None)

        def send(wid, **values):
            wait(lambda: not mailbox.exists())
            pending = mailbox.with_suffix(".tmp")
            pending.write_text(json.dumps(dict(window=wid, **values)))
            pending.replace(mailbox)
            wait(lambda: not mailbox.exists() or not any(w["id"] == wid for w in windows()))
            if mailbox.exists():
                mailbox.unlink()

        def key(wid, code):
            send(wid, input=dict(type="key", key=code, down=True))
            send(wid, input=dict(type="key", key=code, down=False))

        def frame(wid, width, height):
            def ready():
                try:
                    image = ppm(frames / f"{wid}.ppm")
                    metadata = next((w for w in windows() if w["id"] == wid), None)
                    if metadata and image and image[:2] == (width, height):
                        return image
                except FileNotFoundError:
                    pass
                return None
            return wait(ready)

        def sample(image, x, y):
            return tuple(image[2][(y * image[0] + x) * 3:(y * image[0] + x) * 3 + 3])

        checks = []
        try:
            output = guest.shell("sdl-check --surface-only; echo SDL_STATUS:$?", seconds=300)
            (args.output / "guest-surfaces.log").write_text(output)
            assert "SDL_STATUS:0" in output and "SDL_ACCEPTANCE --surface-only passed" in output, output
            actual = records(output)
            if actual != reference:
                diff = "\n".join(difflib.unified_diff(reference, actual, fromfile="upstream", tofile="guest", lineterm=""))
                (args.output / "surface.diff").write_text(diff + "\n")
                raise AssertionError("Upstream/guest SDL pixel mismatch:\n" + diff[:12000])
            (args.output / "surface.diff").unlink(missing_ok=True)
            checks += ["exact upstream/guest logical RGBA", "clipping", "pixel format conversion",
                       "palette", "pitch padding", "color key", "surface/pixel alpha",
                       "overlapping self blits", "BMP memory decode", "BMP stdio roundtrip over 32 KiB",
                       "16/32/64-bit endian RWops", "memory seek/EOF/zero-count bounds"]
            if not args.surface_only:
                output = guest.shell("sdl-check --events; echo SDL_STATUS:$?")
                assert "SDL_STATUS:0" in output and "SDL_ACCEPTANCE --events passed" in output, output
                checks += ["event queue order and peeking", "SDL1 event-filter/state semantics"]
                output = guest.shell("sdl-check --timing; echo SDL_STATUS:$?")
                assert "SDL_STATUS:0" in output and "SDL_ACCEPTANCE --timing passed" in output, output
                checks += ["monotonic ticks and real delay"]
                if not args.skip_profile:
                    output = guest.shell("sdl-check --unsupported; echo SDL_STATUS:$?")
                    assert "SDL_STATUS:0" in output and "SDL_ACCEPTANCE --unsupported passed" in output, output
                    checks += ["explicit errors for unsupported threads/timer callbacks"]
                    output = guest.shell("sdl-check --rwops-profile; echo SDL_STATUS:$?")
                    assert "SDL_STATUS:0" in output and "SDL_ACCEPTANCE --rwops-profile passed" in output, output
                    checks += ["safe RWops negative/overflow requests and seek clamps"]
            if gui:
                guest.shell("sdl-demo --trace &")
                guest.shell("sdl_demo=$!")
                metadata = wait(window)
                wid = metadata["id"]
                wait(lambda: b"SDL_DEMO frame 640 400 selected 2 2 tint 0" in guest.log)
                image = frame(wid, 640, 400)
                assert sample(image, 0, 50) == (24, 28, 34), sample(image, 0, 50)
                assert sample(image, 75, 88) == (132, 160, 184), sample(image, 75, 88)
                (args.output / "initial.ppm").write_bytes((frames / f"{wid}.ppm").read_bytes())
                # Click tile (5,3), then navigate and change the palette by key.
                at = len(guest.log)
                send(wid, input=dict(type="motion", x=425, y=232))
                send(wid, input=dict(type="button", button=0, down=True, x=425, y=232))
                send(wid, input=dict(type="button", button=0, down=False, x=425, y=232))
                wait(lambda: b"selected 5 3 tint 0" in guest.log[at:])
                at = len(guest.log)
                key(wid, 79)
                wait(lambda: b"selected 6 3 tint 0" in guest.log[at:])
                at = len(guest.log)
                key(wid, 44)
                wait(lambda: b"selected 6 3 tint 1" in guest.log[at:])
                pause(.1)
                image = frame(wid, 640, 400)
                assert sample(image, 75, 88) == (210, 135, 88), sample(image, 75, 88)
                # Positive wheel is wheel-up, restoring the first palette.
                at = len(guest.log)
                send(wid, input=dict(type="wheel", x=0, y=65536))
                wait(lambda: b"selected 6 3 tint 0" in guest.log[at:])
                at = len(guest.log)
                send(wid, resize=[800, 480])
                wait(lambda: b"SDL_DEMO frame 800 480 selected 6 3 tint 0" in guest.log[at:])
                image = frame(wid, 800, 480)
                assert sample(image, 0, 50) == (24, 28, 34)
                (args.output / "resized.ppm").write_bytes((frames / f"{wid}.ppm").read_bytes())
                # An idle application must not repeatedly publish unchanged frames.
                pause(.25)
                stamp = (frames / f"{wid}.ppm").stat().st_mtime_ns
                pause(1.2)
                assert (frames / f"{wid}.ppm").stat().st_mtime_ns == stamp, "idle app keeps publishing"
                send(wid, close=True)
                output = guest.shell("wait $sdl_demo; echo SDL_DEMO_STATUS:$?")
                assert "SDL_DEMO_STATUS:0" in output, output
                wait(lambda: not window())
                # Escape is a separate exit route from the WM close event.
                guest.shell("sdl-demo --trace &")
                guest.shell("sdl_demo=$!")
                second = wait(window)["id"]
                frame(second, 640, 400)
                key(second, 41)
                output = guest.shell("wait $sdl_demo; echo SDL_DEMO_STATUS:$?")
                assert "SDL_DEMO_STATUS:0" in output, output
                wait(lambda: not window())
                # Exercise native 16/24-bit modes as well as the 32-bit shadow.
                for depth in (16, 24):
                    at = len(guest.log)
                    guest.shell(f"sdl-demo --trace --depth {depth} &")
                    guest.shell("sdl_demo=$!")
                    direct = wait(window)["id"]
                    wait(lambda: b"SDL_DEMO frame 640 400 selected 2 2 tint 0" in guest.log[at:])
                    image = frame(direct, 640, 400)
                    target = (24, 28, 34)
                    assert all(abs(a-b) <= (7 if depth == 16 else 0)
                               for a, b in zip(sample(image, 0, 50), target)), (depth, sample(image, 0, 50))
                    (args.output / f"depth-{depth}.ppm").write_bytes((frames / f"{direct}.ppm").read_bytes())
                    send(direct, close=True)
                    output = guest.shell("wait $sdl_demo; echo SDL_DEMO_STATUS:$?")
                    assert "SDL_DEMO_STATUS:0" in output, output
                    wait(lambda: not window())
                checks += ["GX full-color frame", "pointer selection", "arrow/space keys", "wheel",
                           "resize", "idle damage", "WM close and escape cleanup", "16/24/32-bit modes"]
            assert not any(marker in guest.log for marker in
                           (b"native runtime error", b"panic:", b"Assertion `", b"graphics operation failed")), \
                guest.log[-6000:].decode(errors="replace")
        finally:
            try:
                guest.close()
            finally:
                (args.output / "guest.log").write_bytes(guest.log)
    report = dict(status="passed", checks=checks, records=len(reference),
                  guest_sha256=hashlib.sha256(args.guest.read_bytes()).hexdigest())
    (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"SDL/Fuse-X: {len(checks)} acceptance checks, exact upstream/guest pixels passed")


if __name__ == "__main__":
    main()
