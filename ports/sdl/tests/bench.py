#!/usr/bin/env python3
"""Measure SDL selection latency in a disposable, headless Fuse-X session.

The elapsed time is from publishing an input event to the demo's frame trace,
which follows drawing and presentation. It includes mailbox polling and the frame
dump path, but excludes window-system display latency. Run the same command
against old/new binaries and use --compare to verify identical frame pixels.
No persistent guest image is opened or modified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "ports/tls/tests"))
from guest import Guest


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def profile_fields(output, prefix):
    """Read the last complete profile preceding a demo frame trace."""
    records = []
    for line in output.decode(errors="replace").replace("\r", "").splitlines():
        if line.startswith(prefix + " "):
            fields = {}
            for key, value in re.findall(r"(\w+)=([^\s]+)", line[len(prefix):]):
                fields[key] = int(value) if re.fullmatch(r"-?\d+", value) else value
            records.append(fields)
    return records[-1] if records else None


def ppm(path, size):
    try:
        data = path.read_bytes()
    except FileNotFoundError:
        return None
    match = re.match(rb"P6\n(\d+) (\d+)\n255\n", data)
    if not match or tuple(map(int, match.groups())) != size:
        return None
    pixels = data[match.end():]
    return (data, pixels) if len(pixels) == size[0] * size[1] * 3 else None


def machine():
    result = dict(system=platform.system(), release=platform.release(),
                  machine=platform.machine(), python=platform.python_version())
    if sys.platform == "darwin":
        for key in ("machdep.cpu.brand_string", "hw.memsize", "hw.logicalcpu"):
            value = subprocess.run(["sysctl", "-n", key], text=True,
                                   capture_output=True)
            if value.returncode == 0:
                result[key] = value.stdout.strip()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--demo", type=Path, default=ROOT / "build/sdl/sdl-demo.fxe")
    parser.add_argument("--output", type=Path, default=ROOT / "build/sdl-performance/current")
    parser.add_argument("--sizes", nargs="+", default=["320x240", "640x400", "1024x800", "1024x1024"])
    parser.add_argument("--depths", nargs="+", type=int, choices=(16, 24, 32), default=[32])
    parser.add_argument("--actions", type=int, default=3)
    parser.add_argument("--full-redraw", action="store_true", help="request full redraw for every selection")
    parser.add_argument("--profile", action="store_true", help="record demo raster/presentation timings")
    parser.add_argument("--gx-profile", action="store_true", help="set guest SDL_PROFILE=1 and record GX timings")
    parser.add_argument("--compare", type=Path, help="previous result.json to compare exact RGB pixels")
    args = parser.parse_args()
    assert args.actions > 0
    sizes = []
    for item in args.sizes:
        match = re.fullmatch(r"(\d+)x(\d+)", item)
        assert match, f"invalid size: {item}"
        size = tuple(map(int, match.groups()))
        assert 320 <= size[0] <= 1024 and 240 <= size[1] <= 1024, item
        sizes.append(size)
    args.output.mkdir(parents=True, exist_ok=True)
    result_path = args.output / "result.json"
    result_path.unlink(missing_ok=True)
    flags = dict(trace=True, full_redraw=args.full_redraw,
                 profile=args.profile, gx_profile=args.gx_profile)
    report = dict(status="running", machine=machine(), flags=flags,
                  measurement="mailbox publication to post-presentation trace; headless frame dumps enabled",
                  demo=str(args.demo.resolve()), demo_sha256=sha256(args.demo.read_bytes()),
                  kernel_sha256=sha256((ROOT / "fuse-x/build/fuse-x").read_bytes()),
                  runs=[])

    with tempfile.TemporaryDirectory(prefix="sdl-bench-") as directory:
        work = Path(directory)
        frames, mailbox = work / "frames", work / "input.json"
        saved = dict(os.environ)
        os.environ.update(FUSE_X_FRAMES=str(frames), FUSE_X_TEST_EVENT=str(mailbox))
        try:
            guest = Guest(work, "native", {"sdl-demo": args.demo},
                          [dict(name="/dev/gfx", mode=0o20600, device=0x400)], blocks=131072)
        finally:
            os.environ.clear()
            os.environ.update(saved)

        def wait(condition, seconds=120):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                guest.pump()
                value = condition()
                if value:
                    return value
                assert guest.process.poll() is None, guest.log[-6000:].decode(errors="replace")
            raise AssertionError("SDL benchmark timed out:\n" + guest.log[-6000:].decode(errors="replace"))

        def windows():
            try:
                return json.loads((frames / "windows.json").read_text())
            except (FileNotFoundError, json.JSONDecodeError):
                return []

        def window():
            return next((w for w in windows() if w["visible"] and w["title"] == "SDL software test"), None)

        def publish(wid, **values):
            wait(lambda: not mailbox.exists())
            pending = mailbox.with_suffix(".tmp")
            pending.write_text(json.dumps(dict(window=wid, **values)))
            start = time.monotonic()
            pending.replace(mailbox)
            return start

        def send(wid, **values):
            publish(wid, **values)
            wait(lambda: not mailbox.exists())

        def trace(width, height, selected_x, at):
            marker = f"SDL_DEMO frame {width} {height} selected {selected_x} 2 tint 0".encode()
            wait(lambda: marker in guest.log[at:])

        def snapshot(wid, size, stem, previous_stamp=None):
            path = frames / f"{wid}.ppm"

            def ready():
                try:
                    if previous_stamp is not None and path.stat().st_mtime_ns == previous_stamp:
                        return None
                    return ppm(path, size)
                except FileNotFoundError:
                    return None

            data, pixels = wait(ready)
            destination = args.output / f"{stem}.ppm"
            destination.write_bytes(data)
            return dict(file=destination.name, pixel_sha256=sha256(pixels))

        try:
            if args.gx_profile:
                guest.shell("SDL_PROFILE=1; export SDL_PROFILE")
            for depth in args.depths:
                for width, height in sizes:
                    at = len(guest.log)
                    options = " --full-redraw" if args.full_redraw else ""
                    if args.profile:
                        options += " --profile"
                    guest.shell(f"sdl-demo --trace --depth {depth}{options} &")
                    guest.shell("sdl_bench=$!")
                    wid = wait(window)["id"]
                    trace(640, 400, 2, at)
                    wait(lambda: ppm(frames / f"{wid}.ppm", (640, 400)))
                    resize_ms = None
                    if (width, height) != (640, 400):
                        at = len(guest.log)
                        start = publish(wid, resize=[width, height])
                        trace(width, height, 2, at)
                        resize_ms = (time.monotonic() - start) * 1000
                    stem = f"depth-{depth}-{width}x{height}"
                    run = dict(depth=depth, width=width, height=height, flags=flags, resize_ms=resize_ms,
                               initial=snapshot(wid, (width, height), stem + "-initial"), actions=[])
                    report["runs"].append(run)
                    for action in range(args.actions):
                        at = len(guest.log)
                        stamp = (frames / f"{wid}.ppm").stat().st_mtime_ns
                        selected_x = (3 + action) % 8
                        start = publish(wid, input=dict(type="key", key=79, down=True))
                        trace(width, height, selected_x, at)
                        elapsed_ms = (time.monotonic() - start) * 1000
                        profile_output = bytes(guest.log[at:])
                        timings = {}
                        if args.profile:
                            timings["profile"] = profile_fields(profile_output, "SDL_DEMO profile")
                            assert timings["profile"] is not None, "demo profile missing"
                        if args.gx_profile:
                            timings["gx_profile"] = profile_fields(profile_output, "SDL_GX_PROFILE")
                            assert timings["gx_profile"] is not None, "GX profile missing"
                        shot = snapshot(wid, (width, height), stem + f"-selection-{action + 1}", stamp)
                        send(wid, input=dict(type="key", key=79, down=False))
                        run["actions"].append(dict(latency_ms=elapsed_ms, selected_x=selected_x,
                                                   **shot, **timings))
                    run["median_ms"] = statistics.median(a["latency_ms"] for a in run["actions"])
                    latencies = ", ".join("%.1f" % a["latency_ms"] for a in run["actions"])
                    print(f"depth={depth} {width}x{height}: median {run['median_ms']:.1f} ms "
                          f"({latencies})", flush=True)
                    send(wid, close=True)
                    output = guest.shell("wait $sdl_bench; echo SDL_BENCH_STATUS:$?")
                    assert "SDL_BENCH_STATUS:0" in output, output
                    wait(lambda: not window())
            assert not any(marker in guest.log for marker in
                           (b"native runtime error", b"panic:", b"Assertion `", b"graphics operation failed")), \
                guest.log[-6000:].decode(errors="replace")
        finally:
            try:
                guest.close()
            finally:
                (args.output / "guest.log").write_bytes(guest.log)

    if args.compare:
        before = json.loads(args.compare.read_text())
        previous = {(r["depth"], r["width"], r["height"]): r for r in before["runs"]}
        for run in report["runs"]:
            old = previous[(run["depth"], run["width"], run["height"])]
            assert run["initial"]["pixel_sha256"] == old["initial"]["pixel_sha256"], "initial pixels changed"
            assert len(run["actions"]) == len(old["actions"]), "different action counts"
            for actual, expected in zip(run["actions"], old["actions"]):
                assert actual["selected_x"] == expected["selected_x"]
                assert actual["pixel_sha256"] == expected["pixel_sha256"], "selection pixels changed"
            run["speedup"] = old["median_ms"] / run["median_ms"]
        report["compared_with"] = str(args.compare.resolve())
    report["status"] = "passed"
    result_path.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Saved {result_path}")


if __name__ == "__main__":
    main()
