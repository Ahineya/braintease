#!/usr/bin/env python3
"""Run the GX_SESSION ABI probe against private native and nested desktops.

This does not rebuild or install a normal image. The supplied kernel and probe
are exercised through the actual guest ioctl, fork/exec, pipe and select paths.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ports/tls/tests"))
from guest import Guest


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def profile(args, nested):
    name = "nested" if nested else "native"
    destination = args.output / name
    destination.mkdir(parents=True, exist_ok=True)
    saved = dict(os.environ)
    guest = None
    with tempfile.TemporaryDirectory(prefix="fuse-x-session-" + name + "-") as directory:
        work = Path(directory)
        frames = work / "frames"
        os.environ.update(FUSE_X_TEST_KERNEL=str(args.kernel),
                          FUSE_X_DESKTOP="1" if nested else "0",
                          FUSE_X_DESKTOP_SIZE="1280x800",
                          FUSE_X_FRAMES=str(frames), RCC_NATIVE_AUDIO="off")
        try:
            guest = Guest(work, "native", {"session-probe": args.probe},
                          [dict(name="/dev/gfx", mode=0o20666, device=0x400)],
                          blocks=32768, build_override=args.build, timeout=300)
            command = "session-probe" + (" --nested" if nested else "")
            output = guest.shell(command + "; echo SESSION_STATUS:$?", seconds=240)
            assert "SESSION_STATUS:0" in output, output
            assert "SESSION_PASSED " + name + " " in output, output
            assert not any(error in guest.log for error in
                           (b"SESSION_FAILED", b"panic:", b"native runtime error",
                            b"graphics operation failed")), guest.log.decode(errors="replace")
            checks = [line.partition("SESSION_CHECK ")[2].strip()
                      for line in output.splitlines() if line.startswith("SESSION_CHECK ")]
            assert len(checks) >= (13 if nested else 12), output
            if nested:
                state = json.loads((frames / "desktop.json").read_text())
                assert not state["windows"] and state["focused"] is None, state
                (destination / "desktop.json").write_text(json.dumps(state, indent=2) + "\n")
            else:
                assert not (frames / "desktop.json").exists(), "Nested desktop unexpectedly enabled"
            closing, guest = guest, None
            closing.close()
            return dict(status="passed", checks=checks, private_image_fsck="passed")
        finally:
            if guest is not None:
                try:
                    guest.close()
                finally:
                    (destination / "guest.log").write_bytes(guest.log)
            elif (work / "guest.log").exists():
                (destination / "guest.log").write_bytes((work / "guest.log").read_bytes())
            os.environ.clear()
            os.environ.update(saved)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=ROOT / "build/fusede-session/fuse-x")
    parser.add_argument("--probe", type=Path, default=ROOT / "build/fusede-session/session-probe.fxe")
    parser.add_argument("--build", type=Path, default=ROOT / "fuse-x/build")
    parser.add_argument("--output", type=Path, default=ROOT / "build/fusede-session/session-tests")
    args = parser.parse_args()
    for name in ("kernel", "probe", "build", "output"):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    receipt = args.output / "result.json"
    receipt.unlink(missing_ok=True)
    started = time.monotonic()
    report = dict(status="running", private_images_only=True, networking="off",
                  kernel_sha256=sha256(args.kernel), probe_sha256=sha256(args.probe),
                  probe_source_sha256=sha256(ROOT / "fuse-x/examples/session.c"),
                  test_source_sha256=sha256(__file__),
                  shared_libraries={path.name: sha256(path)
                                    for path in sorted((args.build / "lib").glob("*.fxl.[0-9]"))},
                  profiles={})
    try:
        for nested in (False, True):
            report["profiles"]["nested" if nested else "native"] = profile(args, nested)
        report["status"] = "passed"
    except BaseException as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        report["elapsed_seconds"] = round(time.monotonic() - started, 3)
        receipt.write_text(json.dumps(report, indent=2) + "\n")
    print("GX_SESSION: real guest registry, metadata, child association, snapshot/watch, "
          "manager controls, authority revocation and native-mode boundaries passed")


if __name__ == "__main__":
    main()
