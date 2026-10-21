#!/usr/bin/env python3
"""Measure real Fuse-X continuous plot drags on a fresh offline disposable disk.

The C16 workload uses the production plot widget, real LVGL pointer events and
GX raster/upload/presentation. Explicit refreshes expose the maximum sustainable
rate independently of the app's 30 Hz scheduler. PPM capture is disabled because
converting and writing multi-megabyte screenshots is acceptance-test overhead.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PORT = Path(__file__).resolve().parent
BUILD = ROOT / "build/lvgl"
sys.path.insert(0, str(ROOT / "ports/tls/tests"))
from guest import Guest


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_workload(output, snapshot=None, gx_source=None, workload_source=None):
    """Reuse main content-addressed IR read-only; keep all mutations local."""
    output = output.resolve()
    snapshot = snapshot.resolve() if snapshot else None
    gx_source = (gx_source or PORT / "gx.c").resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("adapt", PORT / "adapt.py")
    adapt = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adapt)
    source, config = BUILD / "src", BUILD / "config"
    compiler = ROOT / "target/release/rcc"
    includes = [ROOT / "ports/fuzix/ripple/include", ROOT / "ports/fuzix/upstream/Library/include",
                ROOT / "runtime/include", ROOT / "ports/fuzix/ripple", ROOT / "ports/graphics/include",
                source, config, PORT]
    flags = ["--target", "native", "--bank-size", "64000", "-DRIPPLE_MMU=1",
             "-DFUSE_X_USER=1", "-DLV_CONF_INCLUDE_SIMPLE=1", '-D__func__="C16"']
    flags += [x for path in includes for x in ("-I", str(path))]
    digest = hashlib.sha256(str((compiler.stat().st_mtime_ns, flags)).encode())
    for directory in includes:
        for path in sorted(directory.rglob("*.h")):
            digest.update(str(path).encode())
            digest.update(path.read_bytes())
    prefix = digest.digest()
    fonts = json.loads((PORT / "fonts/generated.json").read_text())["sizes"]
    font_units = [PORT / "fonts" / f"font-{n}.c" for n in fonts]
    if 'gx_font_data_open' in gx_source.read_text():
        pack_spec = importlib.util.spec_from_file_location('graphite_font_pack', PORT/'fonts/pack.py')
        font_pack = importlib.util.module_from_spec(pack_spec); pack_spec.loader.exec_module(font_pack)
        font_units = font_pack.prepare(output.parent) + [PORT/'font-data.c']
    units = adapt.sources(source) + font_units
    units += [PORT / "fonts/symbols.c", PORT / "icons/data.c", PORT / "ui-scale.c",
              PORT / "ui.c", PORT / "ui-data.c", PORT / "ui-app.c", PORT / "ui-forms.c", PORT / "browser.c"]
    units += [(snapshot or PORT) / "plot.c"]
    if (PORT / "plot-raster.c").exists():
        units += [PORT / "plot-raster.c"]
    cache = output.parent / "objects"
    cache.mkdir(exist_ok=True)

    def compile_one(path):
        key = hashlib.sha256(prefix + path.read_bytes()).hexdigest()[:20]
        filename = path.stem + "-" + key + ".ir"
        primary = BUILD / "objects" / filename
        if primary.exists():
            return primary
        local = cache / filename
        if not local.exists():
            result = subprocess.run([str(compiler), "compile", str(path), *flags, "--emit-ir"], capture_output=True)
            (cache / (path.stem + ".log")).write_bytes(result.stderr)
            if result.returncode:
                raise RuntimeError(str(path) + "\n" + result.stderr.decode(errors="replace")[-4000:])
            local.write_bytes(result.stdout)
        return local

    with ThreadPoolExecutor(max_workers=4) as pool:
        objects = list(pool.map(compile_one, units))
    workload = (workload_source or PORT / "plot-bench.c").resolve()
    if snapshot:
        workload = output.parent / "plot-bench-baseline.c"
        workload.write_text((workload_source or PORT / "plot-bench.c").read_text().replace('#include "ui-plot.c"',
                            '#include "' + str(snapshot / "ui-plot.c") + '"'))
    command = [sys.executable, str(ROOT / "fuse-x/cc.py"), str(gx_source), str(workload),
               "--static", "--float-format", "-D", "LV_CONF_INCLUDE_SIMPLE=1", "-D", '__func__="C16"', "-o", str(output)]
    for path in (source, config, PORT):
        command += ["-I", str(path)]
    for path in objects:
        command += ["--object-ir", str(path)]
    inputs = {"workload": str((workload_source or PORT / "plot-bench.c").resolve().relative_to(ROOT)),
              "workload_sha256": sha(workload_source or PORT / "plot-bench.c"), "ui-plot.c": sha((snapshot or PORT) / "ui-plot.c"),
              "plot.c": sha((snapshot or PORT) / "plot.c"), "gx.c": sha(gx_source)}
    for path in (PORT / "plot-raster.c", PORT / "plot-raster.h", PORT / "gx-port.h",
                 PORT / 'font-data.c', PORT / 'font-data.h', PORT / 'fonts/pack.py',
                 ROOT / "ports/graphics/include/gx.h"):
        if path.exists():
            inputs[str(path.relative_to(ROOT))] = sha(path)
    inputs["compiler"] = sha(compiler)
    inputs["common_header_digest"] = prefix.hex()
    with output.with_suffix(".build.log").open("w") as log:
        subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    output.with_suffix(".inputs.json").write_text(json.dumps(inputs, indent=2) + "\n")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--gx-source", type=Path, help="Private instrumentation copy of gx.c")
    parser.add_argument("--software-target", action="store_true", help="Use the portable C16 drawing-buffer path")
    parser.add_argument("--require-retained", action="store_true", help="Require native plot targets, retained A8 hits and zero RGB uploads")
    parser.add_argument("--snapshot", type=Path, help="Frozen plot.c/ui-plot.c directory for baseline")
    parser.add_argument("--binary", type=Path, default=BUILD / "plot-perf/plot-bench.fxe")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--kernel", type=Path, help="Alternate test kernel; default FUSE_X_TEST_KERNEL or fuse-x/build/fuse-x")
    parser.add_argument("--size", choices=("800x560", "1470x900", "1920x1080", "all"), default="all")
    parser.add_argument("--scale", choices=("1", "2", "both"), default="both")
    parser.add_argument("--rows", choices=("40", "100000", "both"), default="both")
    parser.add_argument("--frames", type=int, default=32)
    parser.add_argument("--events-per-frame", type=int, default=1)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=1200, help="Seconds allowed per workload, including deliberately slow baselines")
    parser.add_argument("--minimum-fps", type=float, help="Require the after variant to sustain this uncapped rate")
    parser.add_argument("--output", type=Path, default=BUILD / "plot-perf/result.json")
    args = parser.parse_args()
    if not 10 <= args.frames <= 300 or not 1 <= args.events_per_frame <= 40 or args.runs < 1 or args.timeout < 1 or (args.minimum_fps is not None and args.minimum_fps <= 0):
        parser.error("Use 10 to 300 frames, 1 to 40 input events per frame and positive runs")
    if args.require_retained and args.software_target:
        parser.error("--require-retained and --software-target are mutually exclusive")
    if args.build:
        build_workload(args.binary, args.snapshot, args.gx_source)
    binaries = {"after": args.binary}
    if args.baseline:
        binaries["before"] = args.baseline
    hashes = {name: sha(path) for name, path in binaries.items()}
    binary_inputs = {name: json.loads(path.with_suffix(".inputs.json").read_text()) if path.with_suffix(".inputs.json").exists() else None
                     for name, path in binaries.items()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    sizes = ("800x560", "1470x900") if args.size == "all" else (args.size,)
    scales = (1, 2) if args.scale == "both" else (int(args.scale),)
    counts = (40, 100000) if args.rows == "both" else (int(args.rows),)
    results = []
    kernel = (args.kernel or Path(os.environ.get("FUSE_X_TEST_KERNEL", str(ROOT / "fuse-x/build/fuse-x")))).resolve()
    kernel_hash = sha(kernel)
    with tempfile.TemporaryDirectory(prefix="lvplot-drag-") as directory:
        environment = dict(os.environ)
        os.environ["FUSE_X_TEST_KERNEL"] = str(kernel)
        for name in ("FUSE_X_FRAME", "FUSE_X_FRAMES", "FUSE_X_TEST_EVENT"):
            os.environ.pop(name, None)
        try:
            g = Guest(Path(directory), "native", {"plot" + name: path for name, path in binaries.items()},
                      [dict(name="/dev/gfx", mode=0o20600, device=0x400)], blocks=131072 if args.baseline else 65536,
                      timeout=max(900, args.timeout * args.runs * len(sizes) * len(scales) * len(counts) * len(binaries) + 60))
        finally:
            os.environ.clear()
            os.environ.update(environment)
        try:
            for size in sizes:
                width, height = map(int, size.split("x"))
                for scale in scales:
                    for rows in counts:
                        for run in range(args.runs):
                            variants = list(binaries)
                            if run % 2:
                                variants.reverse()
                            for variant in variants:
                                target_environment = "LVGL_SOFTWARE_TARGET=1 " if args.software_target else ""
                                text = g.shell(f"HOME=/tmp LVGL_PROFILE=1 LVGL_SCALE={scale} {target_environment}plot{variant} "
                                               f"{width} {height} {rows} {args.frames} {args.events_per_frame}", seconds=args.timeout)
                                result = dict(variant=variant, size=size, density=scale, rows=rows, run=run)
                                for line in text.splitlines():
                                    if line.startswith(("PLOT_BENCH ", "LVGL_PROFILE ", "GROUP_PROFILE ")):
                                        result[line.split()[0]] = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)}
                                assert "PLOT_BENCH" in result and "LVGL_PROFILE" in result, text
                                if args.require_retained and variant == "after":
                                    profile = result["LVGL_PROFILE"]
                                    assert profile.get("target_batches", 0) > 0 and profile.get("target_commands", 0) > 0, profile
                                    assert profile.get("target_mask_hits", 0) > 0, profile
                                    assert profile["rgb_uploads"] == profile["target_readbacks"] == profile["target_fallbacks"] == 0, profile
                                bench = result["PLOT_BENCH"]
                                result["throughput_fps"] = round(bench["frames"] * 1e6 / bench["wall_us"], 2)
                                result["p95_ms"] = round(bench["p95_us"] / 1000, 3)
                                results.append(result)
                                print(json.dumps(result), flush=True)
                                args.output.write_text(json.dumps(dict(status="running", executable_sha256=hashes, kernel_sha256=kernel_hash, results=results), indent=2) + "\n")
        finally:
            try:
                g.close()
            finally:
                args.output.with_suffix(".log").write_bytes(g.log)
    assert hashes == {name: sha(path) for name, path in binaries.items()}, "Benchmark binary changed during measurement"
    passes = args.minimum_fps is None or all(row["PLOT_BENCH"]["frames"] * 1e6 >=
              args.minimum_fps * row["PLOT_BENCH"]["wall_us"] for row in results if row["variant"] == "after")
    assert kernel_hash == sha(kernel), "Benchmark kernel changed during measurement"
    report = dict(status=("passed" if passes else "failed") if args.minimum_fps is not None else "measured", executable_sha256=hashes, kernel_sha256=kernel_hash, results=results,
                  capture="disabled", scheduler="uncapped explicit refresh; excludes production scheduler wait",
                  binary_input_sha256=binary_inputs, requested_minimum_fps=args.minimum_fps,
                  retained_target="required" if args.require_retained else "disabled" if args.software_target else "automatic")
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    assert passes, "Drag throughput below requested threshold"


if __name__ == "__main__":
    main()
