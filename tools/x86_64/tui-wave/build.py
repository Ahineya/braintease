#!/usr/bin/env python3
"""Build the three existing TUI applications as static C16/RXE1 candidates.

This opt-in wave writes only its build directory. It neither installs apps nor
publishes a catalog, and does not change the supported default application set.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
RFL = {"rfl-fm": "commander", "rift": "rift"}


def sha(path):
    return hashlib.file_digest(path.open("rb"), "sha256").hexdigest()


def build(name, directory):
    apps = directory / "apps"
    logs = directory / "logs"
    programs = directory / "programs"
    for path in (apps, logs, programs):
        path.mkdir(parents=True, exist_ok=True)
    output = apps / (name + ".fxe")
    flags = []
    if name == "mux":
        sources = [ROOT / "ports/fuzix/ripple/mux.c"]
        flags = ["-DUIDGID=0"]
    else:
        source = ROOT / "rfl/apps" / RFL[name] / "main.rfl"
        command = [ROOT / "target/release/rfl", "compile", source,
                   "--emit-ir", "--target", "native"]
        result = subprocess.run(list(map(str, command)), capture_output=True, check=True)
        ir = result.stdout.decode()
        if re.search(r"@(mmio_read|mmio_write|rfl_host_clock_read|rfl_host_idle|rcc_hosted_input)\(", ir):
            raise RuntimeError("standalone host/MMIO imports cannot run as an OS process")
        program = programs / (name + ".ir")
        program.write_text(ir)
        sources = [program, ROOT / "rfl/os/posix.c"]
        if "@rfl_curses_" in ir:
            sources.append(ROOT / "rfl/os/curses.c")
            flags.append("--curses")
        if "@rfl_sftp_" in ir:
            sources.append(ROOT / "ports/ssh/commander.c")
        if "@rfl_text_paint" in ir:
            sources.append(ROOT / "rfl/os/text_curses.c")
        if "@tui_theme_" in ir:
            sources.append(ROOT / "libraries/tui-theme/tui_theme.c")
    command = [sys.executable, ROOT / "fuse-x/cc.py", *sources, *flags,
               "--static", "--heap-profile", "bounded",
               "--linker", ROOT / "tools/x86_64/app_link.py",
               "--build", directory, "-o", output]
    with (logs / (name + ".log")).open("w") as log:
        result = subprocess.run(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT,
                                env=dict(os.environ, RCC_NATIVE_MEMORY="checked"))
    if result.returncode:
        raise RuntimeError(f"{name} failed; inspect {logs / (name + '.log')}")
    data = output.read_bytes()
    if data[:12] != b"FUSEX001RXE1":
        raise RuntimeError("output is not an RXE1 x64 application")
    metadata = json.loads(output.with_suffix(".json").read_text())
    return dict(name=name, path=str(output), bytes=len(data), sha256=sha(output),
                inputs={str(p.relative_to(ROOT)): sha(p) for p in sources},
                metadata=metadata,
                runtime="Compilation only; guest execution is a separate acceptance gate")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apps", nargs="*", choices=("mux", "rfl-fm", "rift"))
    parser.add_argument("--build", type=Path, default=ROOT / "build/x86_64-tui-wave")
    args = parser.parse_args()
    directory = args.build.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    report = directory / "build-result.json"
    results = json.loads(report.read_text()) if report.exists() else []
    for name in args.apps or ("mux", "rfl-fm", "rift"):
        print("Building x64 " + name, flush=True)
        result = build(name, directory)
        results = [r for r in results if r["name"] != name] + [result]
        report.write_text(json.dumps(results, indent=2) + "\n")
    print("Built " + ", ".join(r["name"] for r in results), flush=True)


if __name__ == "__main__":
    main()
