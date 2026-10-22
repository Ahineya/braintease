#!/usr/bin/env python3
"""Exercise the production portable dock model, controller, and raster painters.

This needs only rustc. It changes no guest image, host window or shared runtime.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rustc", default=os.environ.get("RUSTC", "rustc"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/fusede-dock/unit")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = ROOT / "libraries/graphics/desktop_dock_test.rs"
    binary = output / "dock-tests"
    command = shlex.split(args.rustc) + ["--edition=2021", "--test", str(source), "-o", str(binary)]
    files = ["desktop_dock_test.rs", "desktop_dock.rs", "dock.rs", "desktop.rs",
             "desktop_icons.rs", "desktop_text.rs"]
    sources = {str(ROOT / "libraries/graphics" / name): sha256(ROOT / "libraries/graphics" / name)
               for name in files}
    started = time.monotonic()
    report = dict(status="running", source_sha256=sources, runner_sha256=sha256(__file__),
                  compile_command=command, images_changed=False, host_windows_opened=False)
    try:
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=120)
        (output / "compile.log").write_text(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            raise RuntimeError("Dock tests did not compile: " + compiled.stderr)
        report["binary_sha256"] = sha256(binary)
        tested = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        (output / "run.log").write_text(tested.stdout + tested.stderr)
        print(tested.stdout, end="")
        if tested.returncode:
            raise RuntimeError("Dock tests failed: " + tested.stdout + tested.stderr)
        passed = re.search(r"test result: ok\. (\d+) passed; (\d+) failed; (\d+) ignored", tested.stdout)
        if not passed:
            raise RuntimeError("Dock harness did not report completion")
        report.update(status="passed", tests=int(passed[1]), ignored=int(passed[3]))
    except BaseException as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        report["elapsed_seconds"] = round(time.monotonic() - started, 3)
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
