#!/usr/bin/env python3
"""Build and run the Doom engine port on the Rust VM."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from disk_image import prepare_disk

PORT = Path(__file__).resolve().parent
ROOT = PORT.parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", choices=["title", "e1m1"], default="title")
    parser.add_argument("--wad", type=Path, default=ROOT / "c-test/examples/doom/doom1.wad")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--frame", type=int, default=1, help="capture this presentation in headless mode")
    parser.add_argument("--input-script", type=Path)
    parser.add_argument("--output", type=Path, default=PORT / "build/run")
    parser.add_argument("--clock", choices=["paced", "unthrottled", "stepped"])
    parser.add_argument("--max-instructions", type=int, default=100_000_000_000)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    if min(args.frame, args.max_instructions, args.timeout) < 1:
        parser.error("frame, instruction limit and timeout must be positive")
    wad = args.wad.resolve()
    command = [sys.executable, str(PORT / "build.py"), "engine", "--target", "ripple", "--wad", str(wad)]
    if args.scene == "e1m1":
        command.append("--start-map")
    subprocess.run(command, check=True, cwd=ROOT)
    if args.build_only:
        return
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # rvm opens disk images writable. Keep the repository WAD pristine.
    disk = out / "doom1.wad"
    if disk == wad:
        parser.error("output directory must not contain the source WAD")
    disk_metadata = prepare_disk(wad, disk)
    command = [str(ROOT / "target/release/rvm"), str(PORT / "build/ripple/engine/engine.bin"),
               "--bank-size", "64000", "--memory", "16384000", "--disk", str(disk), "--seed", "1",
               "--clock", args.clock or ("unthrottled" if args.headless else "paced"),
               "--report", str(out / (args.scene + ".json"))]
    if args.headless:
        command += ["--headless", "--capture-frame", str(args.frame),
                    "--screenshot", str(out / (args.scene + ".png")),
                    "--raw-frame", str(out / (args.scene + ".rgb565")),
                    "--max-instructions", str(args.max_instructions), "--timeout", str(args.timeout)]
    else:
        command += ["--visual"]
    if args.input_script:
        command += ["--input-script", str(args.input_script.resolve())]
    if args.clock == "stepped":
        command += ["--control-stdin"]
    manifest = json.loads((PORT / "build/manifest.json").read_text())
    manifest.update(disk_image=disk_metadata, scene=args.scene, command=command)
    (out / (args.scene + ".manifest.json")).write_text(json.dumps(manifest, indent=2) + "\n")
    subprocess.run(command, check=True, cwd=ROOT)


if __name__ == "__main__":
    main()
