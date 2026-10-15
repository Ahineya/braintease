#!/usr/bin/env python3
"""Serial, bounded interpreter/JIT comparison. Never builds or runs Doom."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess
import time


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bf", type=Path, default=root / "src/rust-bf/target/release/bf")
    parser.add_argument("--rbf", type=Path, default=root / "target/release/rbf")
    parser.add_argument("--mandelbrot", type=Path, default=root / "src/rust-bf/mandelbrot.bf")
    parser.add_argument("--squares", type=Path, help="Frozen BFHOST C square demo; optional")
    parser.add_argument("--frames", type=int, default=4)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--output", type=Path, default=root / "build/bf-jit/benchmark")
    args = parser.parse_args()
    if args.frames < 1 or args.repetitions < 1 or args.timeout <= 0:
        parser.error("frames, repetitions and timeout must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    cases = [("mandelbrot", args.mandelbrot)]
    if args.squares:
        cases.append(("squares", args.squares))
    for case, source in cases:
        source_sha256 = digest(source.read_bytes())
        reference = None
        for iteration in range(args.repetitions):
            order = [False, True] if iteration % 2 == 0 else [True, False]
            for native in order:
                mode = "jit" if native else "interpreter"
                prefix = args.output / f"{case}-{iteration}-{mode}"
                if case == "mandelbrot":
                    command = [str(args.bf), str(source), "--stat"]
                else:
                    command = [str(args.rbf), str(source), "--headless",
                               "--timeout", str(args.timeout), "--capture-frame", str(args.frames),
                               "--raw-frame", str(prefix) + ".rgb565", "--report", str(prefix) + ".json"]
                if native:
                    command.append("--jit")
                started = time.perf_counter()
                run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     timeout=args.timeout + 5 if case == "squares" else args.timeout)
                process_seconds = time.perf_counter() - started
                prefix.with_suffix(".stdout").write_bytes(run.stdout)
                prefix.with_suffix(".stderr").write_bytes(run.stderr)
                if run.returncode:
                    raise RuntimeError(f"{case}/{mode} failed ({run.returncode}): {run.stderr.decode(errors='replace')}")
                if case == "mandelbrot":
                    stderr = run.stderr.decode()
                    match = re.search(r"Execution time: ([0-9.]+) seconds; ([0-9]+) IR dispatches", stderr)
                    if not match:
                        raise RuntimeError("Missing BF execution statistics")
                    seconds, dispatches = float(match[1]), int(match[2])
                    stable = {"stdout_sha256": digest(run.stdout), "source_sha256": source_sha256}
                    jit = None
                    if native:
                        jit_line = next((line for line in stderr.splitlines() if line.startswith("JIT: ")), None)
                        if jit_line is None:
                            raise RuntimeError("Missing JIT statistics")
                        jit = {key: float(value) if any(c in value for c in ".eE") else int(value)
                               for key, value in re.findall(r"(\w+): ([0-9.eE+\-]+)", jit_line)}
                else:
                    report = json.loads(prefix.with_suffix(".json").read_text())
                    seconds, dispatches, jit = report["elapsed_seconds"], report["bf_dispatches"], report.get("jit")
                    stable = {key: value for key, value in report.items()
                              if key not in ("elapsed_seconds", "bf_dispatches", "jit")}
                    stable["stdout_sha256"] = digest(run.stdout)
                    stable["frame_sha256"] = digest(prefix.with_suffix(".rgb565").read_bytes())
                if reference is None:
                    reference = stable
                if stable != reference:
                    raise RuntimeError(f"Output or stable report mismatch: {case}/{mode}")
                results.append(dict(case=case, mode=mode, iteration=iteration, command=command,
                                    execution_seconds=seconds, process_seconds=process_seconds,
                                    dispatches=dispatches, jit=jit, stable=stable, source_sha256=source_sha256,
                                    timing_scope="parse and execute" if case == "mandelbrot" else "execute, including tape initialization and host callbacks"))
                print(f"{case} {mode}: execution {seconds:.6f}s, process {process_seconds:.6f}s", flush=True)
                (args.output / "measurements.json").write_text(json.dumps(results, indent=2) + "\n")
        times = {mode: statistics.median(row["execution_seconds"] for row in results
                                       if row["case"] == case and row["mode"] == mode)
                 for mode in ("interpreter", "jit")}
        print(f"{case} medians: {times['interpreter']:.6f}s -> {times['jit']:.6f}s "
              f"({times['interpreter'] / times['jit']:.2f}x)", flush=True)


if __name__ == "__main__":
    main()
