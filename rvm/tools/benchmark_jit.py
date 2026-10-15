#!/usr/bin/env python3
"""Compare interpreter and JIT on one unchanged RLINK binary.

Example (from the repository root):
  python3 rvm/tools/benchmark_jit.py ports/doom/build/ripple/engine/engine.bin \
    --frame 1 --runs 3 --vm-args --bank-size 64000 --memory 16384000 \
    --disk ports/doom/build/run/doom1.wad --seed 1 --clock unthrottled

Use --baseline /path/to/previous/rvm to include the old JIT. VM startup and
native compilation are included; frontend compilation and disk copying are not.
Disk images are copied separately for every run. Outputs stay in a temporary
directory; --output saves the measurements and correctness hashes as JSON.
"""

import argparse
import hashlib
import json
import shutil
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def sha256(path):
    if not path.exists():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def measure(executable, args, jit, directory):
    runs = []
    for index in range(args.runs):
        extra = list(args.vm_args)
        # Identical writable disks, without changing the source image.
        for position, option in enumerate(extra):
            if option == "--disk" and position + 1 < len(extra):
                disk = directory / f"disk-{index}.img"
                shutil.copyfile(extra[position + 1], disk)
                extra[position + 1] = str(disk)
            elif option.startswith("--disk="):
                disk = directory / f"disk-{index}.img"
                shutil.copyfile(option.split("=", 1)[1], disk)
                extra[position] = f"--disk={disk}"
        report = directory / f"report-{index}.json"
        stdout = directory / f"stdout-{index}"
        stderr = directory / f"stderr-{index}"
        raw = directory / f"frame-{index}.rgb565"
        command = [str(executable), str(args.binary), "--headless", "--timeout",
                   str(args.timeout), "--report", str(report), *extra]
        if jit:
            command.append("--jit")
        if args.frame is not None:
            command.extend(["--capture-frame", str(args.frame), "--raw-frame", str(raw)])
        started = time.perf_counter()
        with stdout.open("wb") as out, stderr.open("wb") as err:
            try:
                status = subprocess.run(command, stdout=out, stderr=err,
                                        timeout=args.timeout + 5).returncode
            except subprocess.TimeoutExpired:
                status = "external_timeout"
        result = {"seconds": time.perf_counter() - started, "status": status,
                  "stdout_sha256": sha256(stdout), "frame_sha256": sha256(raw)}
        if report.exists():
            result["execution"] = json.loads(report.read_text())
        if status != 0:
            result["stderr"] = stderr.read_text(errors="replace")[-4000:]
            result["stdout_tail"] = stdout.read_text(errors="replace")[-4000:]
        runs.append(result)
    return {"seconds_median": statistics.median(run["seconds"] for run in runs), "runs": runs}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--rvm", type=Path, default=ROOT / "target/release/rvm")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--skip-interpreter", action="store_true")
    parser.add_argument("--frame", type=int)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--vm-args", nargs=argparse.REMAINDER, default=[])
    args = parser.parse_args()
    if args.runs < 1 or args.timeout < 1 or (args.frame is not None and args.frame < 1):
        parser.error("runs, timeout, and frame must be positive")
    args.binary = args.binary.resolve()
    modes = ([] if args.skip_interpreter else [("interpreter", args.rvm, False)]) + [("jit", args.rvm, True)]
    if args.baseline:
        modes.insert(0 if args.skip_interpreter else 1, ("baseline_jit", args.baseline, True))
    results = {"binary_sha256": sha256(args.binary), "cases": {}}
    with tempfile.TemporaryDirectory(prefix="rvm-jit-benchmark-") as temporary:
        for name, executable, jit in modes:
            directory = Path(temporary) / name
            directory.mkdir()
            results["cases"][name] = measure(executable.resolve(), args, jit, directory)
    signatures = []
    for case in results["cases"].values():
        for run in case["runs"]:
            if run["status"] == 0:
                signatures.append((run.get("execution", {}).get("instructions"),
                                   run["stdout_sha256"], run["frame_sha256"]))
    results["completed_outputs_match"] = bool(signatures) and all(s == signatures[0] for s in signatures)
    output = json.dumps(results, indent=2) + "\n"
    if args.output:
        args.output.write_text(output)
    print(output, end="")
    if not results["completed_outputs_match"] or any(run["status"] != 0 for run in results["cases"]["jit"]["runs"]):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
