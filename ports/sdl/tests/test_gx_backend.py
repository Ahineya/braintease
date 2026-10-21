#!/usr/bin/env python3
"""Check GX backend failures, input and presentation without a running guest.

The harness uses the pinned upstream host headers and mocks OS/SDL services.
It deliberately does not link either SDL software renderer. Actual C16 pixel
semantics and native windows are covered by the separate guest acceptance run.
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
from adapt import prepare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build/sdl/gx-backend")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--sanitize", choices=("none", "undefined"), default="undefined")
    args = parser.parse_args()
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    source = prepare(build, fetch=args.fetch, c16=False)
    command = shlex.split(args.cc)
    # The installed linker may predate the default macOS 27 SDK's TAPI format.
    # Respect an explicit SDK; otherwise use the installed compatible SDK.
    compatible_sdk = Path("/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk")
    if sys.platform == "darwin" and not os.environ.get("SDKROOT") and compatible_sdk.exists():
        command += ["-isysroot", str(compatible_sdk)]
    command += ["-std=c99", "-Wall", "-Wextra", "-Werror", "-g"]
    if args.sanitize != "none":
        command += ["-fsanitize=" + args.sanitize, "-fno-sanitize-recover=all"]
    for directory in (source / "include", source / "src/video", source / "src/events",
                      ROOT / "ports/graphics/include"):
        command += ["-I", str(directory)]
    binary = build / "gx-backend-check"
    command += [str(PORT / "tests/gx-backend.c"), "-o", str(binary)]
    subprocess.run(command, check=True, timeout=60)
    result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=15)
    assert result.stdout.strip() == "SDL_GX_MOCK_OK", result.stdout + result.stderr
    if result.stderr:
        raise RuntimeError(result.stderr)
    print(result.stdout.strip())


if __name__ == "__main__":
    main()
