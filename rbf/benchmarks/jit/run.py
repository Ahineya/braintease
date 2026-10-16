#!/usr/bin/env python3
"""Build OR measure small checked C/assembly BF CPU workloads; never run Doom."""
import argparse
import collections
import hashlib
import json
import os
import platform
from pathlib import Path
import re
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
CASES = {"alu": ("asm", 512), **{name: ("c", 2048) for name in
         ("gather", "stack_calls", "wide_math", "sort", "masked_math")}}


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(command, timeout, log=None, env=None):
    try:
        result = subprocess.run([str(x) for x in command], cwd=ROOT, env=env,
                                capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        if log:
            Path(str(log) + ".stdout").write_bytes(error.stdout or b"")
            Path(str(log) + ".stderr").write_bytes(error.stderr or b"")
        raise RuntimeError(f"{command[0]} exceeded {timeout}s; logs: {log}") from error
    if log:
        Path(str(log) + ".stdout").write_bytes(result.stdout)
        Path(str(log) + ".stderr").write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(f"{command[0]} failed ({result.returncode}): {result.stderr.decode(errors='replace')}")
    return result


def build(args):
    # Build and measurement are deliberately separate invocations.
    run(["cargo", "build", "--release", "-p", "rcc-driver", "-p", "rbf", "-p", "rvm"], 600)
    for package in ("ripple-asm", "bf-macro-expander"):
        run(["cargo", "build", "--release", "--manifest-path", ROOT / f"src/{package}/Cargo.toml"], 600)
    manifest = {}
    for name, (extension, bank) in CASES.items():
        prefix = args.inputs / name
        source = ROOT / f"rbf/benchmarks/jit/{name}.{extension}"
        env = dict(os.environ, BANK_SIZE=str(bank), MEMORY_BANKS="4", BF_HOST="1")
        command = [ROOT / "scripts/build-bf.sh", source, prefix]
        run(command, 120, prefix.with_suffix(".build"), env)
        objects = [prefix.with_suffix(".pobj")]
        if extension == "c":
            objects.insert(0, prefix.with_suffix(".crt0.pobj"))
        run([ROOT / "src/ripple-asm/target/release/rlink", *objects, "-f", "binary",
             "--bank-size", bank, "-o", prefix.with_suffix(".bin")], 30)
        assembly = prefix.with_suffix(".asm") if extension == "c" else source
        histogram = collections.Counter()
        for line in assembly.read_text().splitlines():
            text = line.split(";", 1)[0].strip()
            if text and re.fullmatch(r"[A-Z][A-Z0-9]*", text.split()[0]):
                histogram[text.split()[0]] += 1
        manifest[name] = dict(bank_size=bank, memory_banks=4, source=str(source),
                              source_sha256=sha(source), bf_sha256=sha(prefix.with_suffix(".bf")),
                              binary_sha256=sha(prefix.with_suffix(".bin")), assembly_instructions=histogram)
        print(f"built {name}: {sum(histogram.values())} assembly instructions", flush=True)
    (args.inputs / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def profile_summary(path, bf):
    report = json.loads(path.read_text())
    shapes = []
    with bf.open("rb") as stream:
        for site in report["sites"][:12]:
            stream.seek(site["source_offset"])
            # Inspect one bounded loop, never dump an expanded BF image.
            snippet = stream.read(4096).decode("ascii")
            depth, end = 0, None
            for i, c in enumerate(snippet):
                depth += (c == "[") - (c == "]")
                if c == "]" and depth == 0:
                    end = i + 1
                    break
            compact = re.sub(r"\s+", "", snippet[:end] if end else snippet[:128])
            compact = re.sub(r"(>+|<+|\++|-+)", lambda m: m[0][0] + str(len(m[0])), compact)
            shapes.append(dict(site, loop_shape=compact[:512], complete=end is not None and len(compact) <= 512))
    return dict(operations=report["operations"], sites=shapes, evicted_sites=report["evicted_sites"])


def measure(args):
    manifest = json.loads((args.inputs / "manifest.json").read_text())
    disk = args.output / "empty.img"
    disk.write_bytes(bytes(1024)) # Fixtures never access disk; avoid RVM's default user disk.
    variants = [("interpreter", args.rbf, False), ("jit", args.rbf, True)]
    if args.baseline or args.baseline_inputs:
        baseline_binary = args.baseline or args.rbf
        variants = [("before-interpreter", baseline_binary, False), ("before-jit", baseline_binary, True)] + variants
    baseline_manifest = (json.loads((args.baseline_inputs / "manifest.json").read_text())
                         if args.baseline_inputs else manifest)
    results = []
    binaries = {"rbf": args.rbf, "rvm": args.rvm}
    if args.baseline:
        binaries["baseline"] = args.baseline
    environment = dict(platform=platform.platform(), machine=platform.machine(),
                       python=platform.python_version(), inputs=manifest,
                       baseline_inputs=baseline_manifest if args.baseline_inputs else None,
                       binaries={key: dict(path=str(path), sha256=sha(path))
                                 for key, path in binaries.items()})
    (args.output / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
    for name in args.cases or list(manifest):
        metadata = manifest[name]
        prefix = args.inputs / name
        if (sha(Path(metadata["source"])) != metadata["source_sha256"] or
            sha(prefix.with_suffix(".bf")) != metadata["bf_sha256"] or
            sha(prefix.with_suffix(".bin")) != metadata["binary_sha256"]):
            raise RuntimeError(f"{name}: source or image changed; rebuild the suite")
        if args.baseline_inputs:
            before = baseline_manifest[name]
            # Compare CPU/engine changes on the same compiled guest program.
            for field in ("bank_size", "memory_banks", "source_sha256", "binary_sha256"):
                if before[field] != metadata[field]:
                    raise RuntimeError(f"{name}: baseline guest differs ({field})")
            before_prefix = args.baseline_inputs / name
            if (sha(Path(before["source"])) != before["source_sha256"] or
                sha(before_prefix.with_suffix(".bf")) != before["bf_sha256"] or
                sha(before_prefix.with_suffix(".bin")) != before["binary_sha256"]):
                raise RuntimeError(f"{name}: baseline source or image changed")
        ref = args.output / f"{name}-rvm"
        reference = run([args.rvm, prefix.with_suffix(".bin"), "--headless", "--bank-size", metadata["bank_size"],
                         "--memory", metadata["bank_size"] * metadata["memory_banks"], "--disk", disk,
                         "--timeout", args.timeout, "--max-instructions", 1_000_000, "--exit-code",
                         "--report", ref.with_suffix(".json")], args.timeout + 5, ref)
        if reference.stdout != b"Y":
            raise RuntimeError(f"{name}: RVM failed its independent checksum")
        instructions = json.loads(ref.with_suffix(".json").read_text())["instructions"]
        stable = None
        for repetition in range(args.repetitions):
            order = variants if repetition % 2 == 0 else list(reversed(variants))
            for label, binary, jit in order:
                output = args.output / f"{name}-{label}-{repetition}"
                selected = ((args.baseline_inputs / name)
                            if args.baseline_inputs and label.startswith("before-") else prefix)
                command = [binary, selected.with_suffix(".bf"), "--headless", "--timeout", args.timeout,
                           "--max-instructions", 1_000_000, "--max-dispatches", 500_000_000,
                           "--exit-code", "--report", output.with_suffix(".json")]
                if jit:
                    command.append("--jit")
                begun = time.perf_counter()
                result = run(command, args.timeout + 5, output)
                wall = time.perf_counter() - begun
                report = json.loads(output.with_suffix(".json").read_text())
                current = {k: v for k, v in report.items() if k not in ("jit", "bf_dispatches", "elapsed_seconds")}
                if args.baseline_inputs:
                    current.pop("source_sha256", None) # Each frozen image's hash was checked above.
                if result.stdout != b"Y" or report["guest_result"] != 0 or report["ripple_instructions"] != instructions:
                    raise RuntimeError(f"{name}/{label}: checksum, return value or guest instruction mismatch")
                if stable is not None and current != stable:
                    raise RuntimeError(f"{name}/{label}: stable report fields changed")
                stable = current
                results.append(dict(case=name, mode=label, repetition=repetition,
                                    seconds=report["elapsed_seconds"], process_seconds=wall,
                                    dispatches=report["bf_dispatches"], instructions=instructions,
                                    jit=report.get("jit"), command=[str(x) for x in command]))
                (args.output / "measurements.json").write_text(json.dumps(results, indent=2) + "\n")
                print(f"{name} {label}: {report['elapsed_seconds']:.6f}s", flush=True)
        if args.profile:
            output = args.output / f"{name}-profile"
            run([args.rbf, prefix.with_suffix(".bf"), "--headless", "--timeout", args.timeout,
                 "--max-instructions", 1_000_000, "--max-dispatches", 500_000_000,
                 "--exit-code", "--profile", output.with_suffix(".json")], args.timeout + 5, output)
            summary = profile_summary(output.with_suffix(".json"), prefix.with_suffix(".bf"))
            output.with_suffix(".summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        medians = {label: statistics.median(r["seconds"] for r in results if r["case"] == name and r["mode"] == label)
                   for label, _, _ in variants}
        print(f"{name} medians: {medians}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true", help="Build tools and fixtures, then exit")
    parser.add_argument("--inputs", type=Path, default=ROOT / "build/bf-jit-cases")
    parser.add_argument("--output", type=Path, default=ROOT / "build/bf-jit-cases/results")
    parser.add_argument("--rbf", type=Path, default=ROOT / "target/release/rbf")
    parser.add_argument("--baseline", type=Path, help="Optional saved older RBF executable")
    parser.add_argument("--baseline-inputs", type=Path,
                        help="Optional frozen BF images from an older CPU; guest binaries must match")
    parser.add_argument("--rvm", type=Path, default=ROOT / "target/release/rvm")
    parser.add_argument("--case", dest="cases", action="append", choices=CASES)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=20)
    parser.add_argument("--profile", action="store_true", help="Collect separate untimed profiles and loop shapes")
    args = parser.parse_args()
    if args.repetitions < 1 or not 0 < args.timeout <= 300:
        parser.error("repetitions must be positive and timeout must be in (0,300]")
    args.inputs = args.inputs.resolve()
    args.output = args.output.resolve()
    args.rbf = args.rbf.resolve()
    args.rvm = args.rvm.resolve()
    if args.baseline: args.baseline = args.baseline.resolve()
    if args.baseline_inputs: args.baseline_inputs = args.baseline_inputs.resolve()
    args.inputs.mkdir(parents=True, exist_ok=True)
    args.output.mkdir(parents=True, exist_ok=True)
    if args.build: build(args)
    else: measure(args)


if __name__ == "__main__":
    main()
