#!/usr/bin/env python3
"""Compare RFL compiler binaries on fixed inputs; optionally execute their output.

python3 rfl/tools/benchmark.py --baseline /path/to/old/rfl --execute
All generated files live in a temporary directory. Compiler builds are excluded.
"""

import argparse
import json
import re
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def command(args):
    return subprocess.run([str(a) for a in args], capture_output=True, text=True)


def measure(compiler, source, directory, runs, execute, compile_args=()):
    asm = directory / "program.asm"
    times = []
    for _ in range(runs):
        start = time.perf_counter()
        compiled = command([compiler, "compile", source, "-o", asm, *compile_args])
        times.append(time.perf_counter() - start)
        if compiled.returncode:
            return {"compile_error": compiled.stderr}
    result = {
        "compile_seconds_median": round(statistics.median(times), 6),
        "assembly_instructions": sum(
            bool(re.match(r"^\s+[A-Z][A-Z0-9]*\s", line))
            for line in asm.read_text().splitlines()
        ),
    }
    if not execute:
        return result
    assembler = ROOT / "src/ripple-asm/target/release"
    obj = directory / "program.pobj"
    binary = directory / "program.bin"
    report = directory / "report.json"
    for args in [
        [assembler / "rasm", "assemble", asm, "-o", obj,
         "--bank-size", "64000", "--max-immediate", "65535"],
        [assembler / "rlink", ROOT / "runtime/crt0.pobj", obj,
         ROOT / "runtime/libruntime.par", "-f", "binary", "--bank-size", "64000", "-o", binary],
    ]:
        output = command(args)
        if output.returncode:
            result["tool_error"] = output.stdout + output.stderr
            return result
    execution_times = []
    for _ in range(runs):
        start = time.perf_counter()
        output = command([ROOT / "target/release/rvm", binary, "--headless", "--exit-code",
                          "--timeout", "20", "--memory", "4294967296", "--report", report])
        execution_times.append(time.perf_counter() - start)
        result.update(status=output.returncode, stdout=output.stdout)
        if report.exists():
            result["execution"] = json.loads(report.read_text())
        if output.returncode:
            result["runtime_error"] = output.stderr
            break
    # Includes VM process startup, excludes compilation, assembly and linking.
    result["vm_seconds_median"] = round(statistics.median(execution_times), 6)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, default=ROOT / "target/release/rfl")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--case", type=Path, action="append", help="Source to measure; repeat for multiple cases")
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    results = {}
    cases = args.case or [ROOT / "rfl/benchmarks/scalars.rfl", ROOT / "rfl/benchmarks/strings.rfl",
             ROOT / "rfl/examples/bf-fn.rfl"]
    with tempfile.TemporaryDirectory(prefix="rfl-benchmark-") as temporary:
        for source in cases:
            results[source.name] = {}
            for name, compiler in [("baseline", args.baseline), ("current", args.compiler)]:
                if compiler is not None:
                    directory = Path(temporary) / source.stem / name
                    directory.mkdir(parents=True)
                    results[source.name][name] = measure(compiler.resolve(), source, directory,
                                                        args.runs, args.execute)
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
