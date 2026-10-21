#!/usr/bin/env python3
"""Check exact demo pixels and damage bounds using the pinned host SDL core.

The real demo painter runs in both incremental and full-repaint modes. This
checks application damage independently of guest pixel-format adaptations and
native presentation, which have separate acceptance tests.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
sys.path.insert(0, str(PORT))
from adapt import COMMON, prepare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build/sdl/demo-damage")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--sanitize", choices=("none", "undefined"), default="undefined")
    args = parser.parse_args()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    source = prepare(build, fetch=args.fetch, c16=False)
    units = [source / name for name in COMMON if not name.endswith("SDL_string.c")]
    units += [PORT / "platform/support.c", PORT / "platform/host.c"]
    units += [source / "src/video" / name for name in
              ("SDL_blit_0.c", "SDL_blit_1.c", "SDL_blit_N.c", "SDL_blit_A.c", "SDL_RLEaccel.c")]
    units += [source / "src/video/dummy" / name for name in
              ("SDL_nullvideo.c", "SDL_nullevents.c", "SDL_nullmouse.c")]
    includes = [source / name for name in ("include", "src", "src/video", "src/events", "src/timer")]
    includes += [PORT / "platform", PORT / "c16"]
    command = shlex.split(args.cc)
    compatible_sdk = Path("/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk")
    if sys.platform == "darwin" and not os.environ.get("SDKROOT") and compatible_sdk.exists():
        command += ["-isysroot", str(compatible_sdk)]
    command += ["-std=c99", "-O2", "-fno-strict-aliasing"]
    command += [part for directory in includes for part in ("-I", str(directory))]
    binary = build / "demo-damage-check"
    objects = build / "objects"
    objects.mkdir(exist_ok=True)
    linked = []
    with (build / "build.log").open("w") as log:
        # Keep upstream's ordinary reference build flags. Its SDL_ResetMouse
        # has a known SDL_BUTTON(0) negative shift during mode initialization;
        # instrument the actual demo painter and harness without suppressing
        # any checks in that code or changing the pinned upstream reference.
        for unit in units:
            obj = objects / (unit.stem + ".o")
            result = subprocess.run(command + ["-c", str(unit), "-o", str(obj)],
                                    stdout=log, stderr=subprocess.STDOUT, timeout=120)
            if result.returncode:
                break
            linked.append(str(obj))
        if not result.returncode:
            if args.sanitize != "none":
                command += ["-fsanitize=" + args.sanitize, "-fno-sanitize-recover=all"]
            command += [str(PORT / "tests/demo-damage.c"), *linked, "-o", str(binary)]
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    if result.returncode:
        raise SystemExit((build / "build.log").read_text()[-6000:])
    result = subprocess.run([str(binary)], capture_output=True, text=True,
                            env=dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy"),
                            timeout=60)
    if result.returncode:
        raise SystemExit(result.stdout + result.stderr)
    expected = "SDL_DEMO_DAMAGE_OK comparisons=507 noops=441"
    assert result.stdout.strip() == expected, result.stdout + result.stderr
    if result.stderr:
        raise RuntimeError(result.stderr)
    print(result.stdout.strip())


if __name__ == "__main__":
    main()
