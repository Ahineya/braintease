#!/usr/bin/env python3
"""Test actual GX_SESSION kernel policy with host dependency stubs.

This checks notification coalescing and manager authority failure atomicity.
Native C sizes deliberately do not stand in for C16 ABI marshalling; the real
guest session probe checks that separately. No kernel build or image is changed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/fusede-session/unit")
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    source = ROOT / "fuse-x/tests/session_policy.c"
    binary = args.output / "session-policy"
    command = shlex.split(args.cc)
    # Match existing host harnesses when the linker predates the default SDK's
    # TAPI format. An explicitly selected SDK remains authoritative.
    compatible_sdk = Path("/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk")
    if sys.platform == "darwin" and not os.environ.get("SDKROOT") and compatible_sdk.exists():
        command += ["-isysroot", str(compatible_sdk)]
    command += ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
              "-I", str(ROOT / "ports/graphics/include"), str(source), "-o", str(binary)]
    started = time.monotonic()
    report = dict(status="running", policy_source_sha256=sha256(ROOT / "fuse-x/session-abi.h"),
                  header_sha256=sha256(ROOT / "ports/graphics/include/gx-session.h"),
                  test_source_sha256=sha256(source), runner_sha256=sha256(__file__),
                  compile_command=command, host_policy_only=True, images_changed=False)
    try:
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
        (args.output / "compile.log").write_text(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            raise RuntimeError("Policy harness compilation failed: " + compiled.stderr)
        report["binary_sha256"] = sha256(binary)
        tested = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        (args.output / "run.log").write_text(tested.stdout + tested.stderr)
        if tested.returncode:
            raise RuntimeError("Policy harness failed: " + tested.stdout + tested.stderr)
        passed = re.search(r"^SESSION_POLICY_PASSED (\d+)$", tested.stdout, re.MULTILINE)
        if not passed:
            raise RuntimeError("Policy harness did not report completion")
        report.update(status="passed", checks=int(passed[1]), cases=[
            line.removeprefix("SESSION_POLICY_CHECK ") for line in tested.stdout.splitlines()
            if line.startswith("SESSION_POLICY_CHECK ")])
        print(tested.stdout, end="")
    except BaseException as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        report["elapsed_seconds"] = round(time.monotonic() - started, 3)
        (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
