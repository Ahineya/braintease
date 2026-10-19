#!/usr/bin/env python3
"""Measure reusable chains or sequential traversal with independently owned results.

Build the release tools and runtime first. Guest instruction counts are the
portable metric; elapsed execution time excludes compilation and linking.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
CASES = {
    "chain": ("reusable_chain.rfl", "repeat/cancel\n", 600_000, 24),
    "traversal": ("traversal.rfl", "traversed twice\n", 400_000, 25),
}


def command(args):
    result = subprocess.run(
        [str(arg) for arg in args], cwd=ROOT, capture_output=True, text=True, timeout=180
    )
    if result.returncode:
        raise RuntimeError(f"{args[0]} failed:\n{result.stdout}{result.stderr}")
    return result


def measure(depth, directory, runtime, case):
    template, expected, _, _ = CASES[case]
    source = directory / f"{case}-{depth}.rfl"
    asm, obj, binary, report = [source.with_suffix(ext) for ext in (".asm", ".pobj", ".bin", ".json")]
    template = ROOT / "rfl/tests/fixtures" / template
    source.write_text(template.read_text().replace("const DEPTH: u16 = 64;", f"const DEPTH: u16 = {depth};"))
    command([ROOT / "target/release/rfl", "compile", source, "-o", asm])
    command([ROOT / "src/ripple-asm/target/release/rasm", "assemble", asm, "-o", obj,
             "--bank-size", "64000", "--max-immediate", "65535"])
    command([ROOT / "src/ripple-asm/target/release/rlink", ROOT / "runtime/crt0.pobj", obj, runtime,
             "-f", "binary", "--bank-size", "64000", "-o", binary])
    started = time.perf_counter()
    result = command([ROOT / "target/release/rvm", binary, "--headless", "--exit-code",
                      "--memory", "16384000", "--timeout", "120", "--report", report])
    seconds = time.perf_counter() - started
    data = json.loads(report.read_text())
    if result.stdout != expected or not data["success"] or data["stop_reason"] != "halted":
        raise RuntimeError(f"Incorrect {case} execution: {result.stdout!r}, {data}")
    return {"case": case, "depth": depth, "instructions": data["instructions"], "seconds": round(seconds, 6)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=CASES, default="chain")
    parser.add_argument("--depths", nargs="+", type=int, default=[64, 128, 256])
    parser.add_argument("--runtime", type=Path, default=ROOT / "runtime/libruntime.par")
    parser.add_argument("--report", type=Path, help="Save the measurements as JSON")
    parser.add_argument("--check", action="store_true", help="Enforce instruction and growth budgets")
    args = parser.parse_args()
    if any(depth < 1 or depth > 6000 for depth in args.depths):
        parser.error("depths must be between 1 and 6000")
    rows = []
    print("depth   guest instructions   execution seconds", flush=True)
    with tempfile.TemporaryDirectory(prefix="rfl-effects-bench-") as tmp:
        for depth in args.depths:
            row = measure(depth, Path(tmp), args.runtime.resolve(), args.case)
            rows.append(row)
            print(f'{depth:5}   {row["instructions"]:18,}   {row["seconds"]:.3f}', flush=True)
    if args.report:
        args.report.write_text(json.dumps(rows, indent=2) + "\n")
    if args.check:
        _, _, per_item, growth = CASES[args.case]
        for row in rows:
            if row["instructions"] > row["depth"] * per_item + 2_000_000:
                raise SystemExit(f'Instruction budget exceeded at depth {row["depth"]}')
        ordered = sorted(rows, key=lambda row: row["depth"])
        for small, large in zip(ordered, ordered[1:]):
            if large["depth"] == small["depth"] * 2 and large["instructions"] * 10 >= small["instructions"] * growth:
                raise SystemExit(f"Superlinear {args.case} cost: {small} -> {large}")


if __name__ == "__main__":
    main()
