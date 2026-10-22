#!/usr/bin/env python3
"""Build and run the restricted macOS USB workbench, preserving each transcript.

Unlike the other research tools this opens the connected controller. With no
options it only reads descriptors, firmware info and IMU calibration. Optional
phases send the specific query/control packets accepted by usb_policy.h.
"""
import argparse
import datetime
import fcntl
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--queries", action="store_true", help="Query NVS status, stick calibration and touchpad information")
    parser.add_argument("--lengths", action="store_true", help="Read firmware-info prefixes with bounded buffer lengths")
    parser.add_argument("--query-shapes", action="store_true", help="Test known query framing and consume-on-read replies")
    parser.add_argument("--controls", action="store_true", help="Brief dim LED and low rumble, followed by stop packets")
    parser.add_argument("--extra-reports", action="store_true", help="GET only: sample descriptor-declared reports 0x0b/0x0c; semantics unknown")
    parser.add_argument("--telemetry", action="store_true", help="Read Sony's traced six-chunk factory telemetry response")
    parser.add_argument("--identity", action="store_true", help="Read published MCU/board identity queries (raw identifiers stay in local log)")
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("USB workbench requires macOS IOKit")
    root = pathlib.Path(__file__).resolve().parent
    output = root / "analysis"
    output.mkdir(exist_ok=True)
    compiler = shutil.which("clang")
    if not compiler:
        parser.error("clang is required")
    with (output / "usb-probe.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            parser.error("another workbench run is active")
        binary = output / "usb-probe"
        command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror"]
        sdk = pathlib.Path("/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX15.5.sdk")
        if sdk.is_dir():
            command += ["-isysroot", str(sdk)]
        command += [str(root / "usb_probe.c"), "-framework", "IOKit", "-framework", "CoreFoundation", "-o", str(binary)]
        subprocess.run(command, check=True, timeout=60)
        flags = ["--" + name.replace("_", "-") for name, enabled in vars(args).items() if enabled]
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        log = output / f"usb-run-{stamp}.jsonl"
        errors = output / f"usb-run-{stamp}.stderr.txt"
        metadata = {
            "started_utc": stamp,
            "flags": flags,
            "build_command": command,
            "sources_sha256": {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                               for name in ("usb_probe.c", "usb_policy.h", "usb_probe.py")},
        }
        manifest = output / f"usb-run-{stamp}.json"
        manifest.write_text(json.dumps(metadata, indent=2) + "\n")
        print(f"USB transcript: {log}", flush=True)
        try:
            with log.open("x") as stream, errors.open("x") as stderr:
                result = subprocess.run([str(binary), *flags], stdout=stream, stderr=stderr, timeout=30)
            metadata["returncode"] = result.returncode
        except subprocess.TimeoutExpired:
            metadata["error"] = "30 second deadline exceeded; child stopped"
            metadata["returncode"] = 1
        finally:
            metadata["transcript_sha256"] = hashlib.sha256(log.read_bytes()).hexdigest()
            manifest.write_text(json.dumps(metadata, indent=2) + "\n")
        if errors.exists() and errors.stat().st_size:
            print(errors.read_text(), file=sys.stderr, end="")
        if metadata.get("error"):
            print(metadata["error"], file=sys.stderr)
        print(f"USB workbench exit status: {metadata.get('returncode', 1)}")
        return metadata.get("returncode", 1)


if __name__ == "__main__":
    sys.exit(main())
