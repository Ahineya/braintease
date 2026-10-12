#!/usr/bin/env python3
"""Reproducible native/Ripple builds of the pinned Doom port.

All upstream edits are applied to an ignored build copy. No vendor checkout is
modified. The WAD stays external to the executable and is never downloaded.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

PORT = Path(__file__).resolve().parent
ROOT = PORT.parent.parent
BUILD = PORT / "build"
BANK_SIZE = 64000
MEMORY_CELLS = 256 * BANK_SIZE
NATIVE_FIXTURES = {"map_decode", "world_widths", "clock_widths"}


def run(command, log=None, cwd=ROOT):
    command = [str(arg) for arg in command]
    if log:
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("w") as output:
            result = subprocess.run(command, cwd=cwd, stdout=output,
                                    stderr=subprocess.STDOUT)
        if result.returncode:
            print(log.read_text()[-16000:], file=sys.stderr)
            raise RuntimeError(f"command failed ({result.returncode}); see {log}")
    else:
        subprocess.run(command, cwd=cwd, check=True)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def native_flags():
    flags = shlex.split(os.environ.get("CFLAGS", ""))
    # Some Macs have a newer Command Line Tools SDK than the selected Xcode
    # linker can read. Prefer the SDK bundled with the compiler itself.
    if sys.platform == "darwin" and not any("sysroot" in f for f in flags):
        cc = os.environ.get("CC", "clang")
        result = subprocess.run([cc, "-print-resource-dir"], text=True, capture_output=True)
        if result.returncode == 0:
            for parent in Path(result.stdout.strip()).parents:
                relative = None
                if parent.name == "Developer":
                    relative = "Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk"
                elif parent.name == "CommandLineTools":
                    relative = "SDKs/MacOSX.sdk"
                if relative and (parent / relative).exists():
                    flags += ["-isysroot", str(parent / relative)]
                    break
    return flags


def prepare(wad):
    run([sys.executable, ROOT / "scripts/fetch-doom-sources.py", "--check"])
    lock = json.loads((ROOT / "vendor/doom-sources.json").read_text())
    patches = sorted((PORT / "patches").glob("*.patch"))
    recipe = {"sources": lock, "patches": {p.name: digest(p) for p in patches}}
    dest = BUILD / "upstream"
    stamp = BUILD / "prepared.json"
    BUILD.mkdir(parents=True, exist_ok=True)
    if not dest.is_dir() or not stamp.exists() or json.loads(stamp.read_text()) != recipe:
        if dest.exists():
            shutil.rmtree(dest)
        shutil.copytree(ROOT / "vendor/doomgeneric", dest,
                        ignore=shutil.ignore_patterns(".git", "build"))
        for patch in patches:
            run(["patch", "--batch", "-p1", "-i", patch],
                BUILD / "logs" / (patch.stem + ".log"), dest)
        stamp.write_text(json.dumps(recipe, indent=2) + "\n")
    generated = BUILD / "generated"
    generated.mkdir(exist_ok=True)
    config = ("/* Generated: lengths are serialized octets, not sizeof units. */\n"
              "#ifndef DOOM_BUILD_CONFIG_H\n#define DOOM_BUILD_CONFIG_H\n"
              f"#define DOOM_WAD_LENGTH {wad.stat().st_size}UL\n"
              f"#define DOOM_WAD_PATH {json.dumps(str(wad))}\n"
              f"#define DOOM_WAD_SHA256 {json.dumps(digest(wad))}\n"
              "#endif\n")
    (generated / "doom_build_config.h").write_text(config)
    manifest = dict(recipe, wad={"path": str(wad), "octets": wad.stat().st_size,
                                "sha256": digest(wad)}, bank_size=BANK_SIZE,
                    memory_cells=MEMORY_CELLS)
    (BUILD / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return dest / "doomgeneric", generated


def sources_for(program, upstream, generated):
    if program == "keyboard_controls":
        run([sys.executable, PORT / "tools/extract_input_fixture.py", upstream / "g_game.c", generated])
        return [PORT / "tests/input_commands.c", PORT / "src/config_port.c"]
    if program == "event_widths":
        return [PORT / "tests/event_widths.c"]
    if program == "sky_widths":
        return [PORT / "tests/sky_widths.c", upstream / "r_sky.c"]
    if program == "memory_kernels":
        return [PORT / "tests/memory_kernels.c", PORT / "src/memory_fast.asm"]
    if program == "raster_kernels":
        return [PORT / "tests/raster_kernels.c", PORT / "src/raster_fast.asm"]
    if program == "render_kernels":
        return [PORT / "tests/render_kernels.c", PORT / "src/render_fast.asm"]
    if program == "wad_lookup":
        return [PORT / "tests/wad_lookup.c"] + [upstream / name for name in (
            "w_wad.c", "w_file.c", "z_zone.c")] + [PORT / "src" / name for name in (
            "wad_io.c", "w_file_port.c")]
    if program == "projection_widths":
        run([sys.executable, PORT / "tools/extract_projection_fixture.py", upstream / "r_segs.c", generated])
        return [PORT / "tests/projection_widths.c", upstream / "m_fixed.c"]
    if program == "texture_columns":
        run([sys.executable, PORT / "tools/extract_texture_fixture.py", upstream / "r_data.c", generated])
        return [PORT / "tests/texture_columns.c"]
    if program == "info_storage":
        run([sys.executable, PORT / "tools/generate_info.py", upstream / "info.c", generated])
        return [PORT / "tests/info_storage.c"] + sorted(generated.glob("info_fill_*.c")) + [generated / "info_init.c"]
    if program in ("numeric", "engine"):
        run([sys.executable, PORT / "tools/generate_tables.py",
             upstream / "tables.c", generated])
        tables = sorted(generated.glob("tables_*.c"))
        if program == "numeric":
            return [PORT / "tests/numeric.c", upstream / "m_fixed.c", upstream / "tables.c"] + tables
        source_line = re.search(r"^SRC_DOOM = (.*)$", (upstream / "Makefile").read_text(), re.M).group(1)
        excluded = {"i_system", "i_timer", "i_input", "i_video", "m_config",
                    "w_file_stdc", "doomgeneric", "doomgeneric_xlib",
                    "i_scale", "f_wipe", "sha1", "w_checksum"}
        engine = [upstream / (Path(obj).stem + ".c") for obj in source_line.split()
                  if Path(obj).stem not in excluded]
        run([sys.executable, PORT / "tools/generate_info.py", upstream / "info.c", generated])
        info = sorted(generated.glob("info_fill_*.c")) + [generated / "info_init.c"]
        return engine + tables + info + [PORT / "src" / name for name in (
            "main.c", "platform.c", "config_port.c", "wad_io.c", "w_file_port.c", "patch_cache.c",
            "render_fast.asm", "raster_fast.asm", "memory_fast.asm")]
    if program == "assets":
        return [PORT / "tests/assets.c", PORT / "src/wad_io.c"]
    if program == "graphics_cache":
        return [PORT / "tests/graphics_cache.c", PORT / "src/patch_cache.c", PORT / "src/wad_io.c"]
    if program == "demo_playback":
        return [PORT / "tests/demo_playback.c", PORT / "src/wad_io.c"]
    if program == "stat_storage":
        return [PORT / "tests/stat_storage.c"]
    if program == "libc":
        return [PORT / "tests/compat_libc.c"]
    if program in NATIVE_FIXTURES:
        dependencies = {
            "map_decode": ["m_fixed.c"],
            "world_widths": ["p_maputl.c", "m_bbox.c", "m_fixed.c"],
            "clock_widths": [],
        }
        return [PORT / "tests" / (program + ".c")] + [
            upstream / name for name in dependencies[program]]
    raise RuntimeError(f"program not implemented: {program}")


def build(program, target, sources, upstream, generated, keep_going=False, start_map=False, jobs=4, low_detail=False):
    out = BUILD / target / program
    out.mkdir(parents=True, exist_ok=True)
    includes = [PORT / "src", generated, upstream]
    definitions = ["DOOM_GENERATED_TABLES=1", "DOOMGENERIC_RESX=320",
                   "DOOMGENERIC_RESY=200", "DOOM_PORT=1"]
    if program in ("engine", "info_storage"):
        definitions.append("DOOM_GENERATED_INFO=1")
    if target == "native":
        if start_map:
            definitions.append("DOOM_START_MAP=1")
        if low_detail:
            definitions.append("DOOM_LOW_DETAIL=1")
        binary = out / program
        command = [os.environ.get("CC", "clang"), "-std=c99", "-O2", "-g",
                   "-Wall", "-Wextra", "-Wno-unused-parameter", "-fwrapv"]
        command += native_flags()
        if program in NATIVE_FIXTURES:
            command += ["-ffunction-sections", "-fdata-sections",
                        "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"]
        for path in includes:
            command += ["-I", path]
        command += ["-D" + definition for definition in definitions]
        command += [source for source in sources if source.suffix == ".c"] + ["-o", binary]
        run(command, out / "build.log")
        return binary
    tools = {name: ROOT / location for name, location in {
        "rcc": "target/release/rcc", "rasm": "src/ripple-asm/target/release/rasm",
        "rlink": "src/ripple-asm/target/release/rlink"}.items()}
    for name, path in tools.items():
        if not path.is_file():
            raise RuntimeError(f"missing {name}: {path}; build the release toolchain first")
    run(["make", "-C", ROOT / "runtime", "all", f"BANK_SIZE={BANK_SIZE}"],
        BUILD / "logs/runtime.log")
    includes.insert(0, PORT / "include")
    includes.append(ROOT / "runtime/include")
    definitions += ["RIPPLE=1", f"BANK_SIZE={BANK_SIZE}"]
    if program in ("engine", "memory_kernels"):
        definitions.append("DOOM_FAST_MEMORY=1")
    if any(source.name == "m_fixed.c" for source in sources):
        sources = sources + [PORT / "src/fixed_fast.asm"]
    if program in ("engine", "libc", "wad_lookup"):
        sources = sources + [PORT / "src/compat_libc.c"]
    objects = []
    failures = []
    header_hash = hashlib.sha256()
    for directory in includes:
        for header in sorted(directory.rglob("*.h")):
            header_hash.update(str(header).encode())
            header_hash.update(header.read_bytes())
    # These fixtures compile excerpts of the prepared engine, not a separate
    # source object. Changes to the excerpt must invalidate the fixture too.
    fixture_includes = {"projection_widths": ("projection_loop.inc",),
                        "texture_columns": ("texture_cache.inc",),
                        "keyboard_controls": ("input_commands.inc", "controls_defaults.inc",
                                              "platform_keyboard.inc")}.get(program, ())
    for fixture_include in fixture_includes:
        header_hash.update((generated / fixture_include).read_bytes())
    tool_key = {name: [path.stat().st_mtime_ns, path.stat().st_size]
                for name, path in tools.items()}
    def compile_source(source):
        # Tests and implementations can have the same basename.
        stem = ("test_" if source.parent == PORT / "tests" else "") + source.stem
        asm = out / (stem + ".asm")
        obj = out / (stem + ".pobj")
        stamp = out / (stem + ".build.json")
        source_definitions = definitions + (["DOOM_START_MAP=1"]
            if start_map and source == PORT / "src/main.c" else [])
        if low_detail and source == PORT / "src/main.c":
            source_definitions.append("DOOM_LOW_DETAIL=1")
        recipe = {"source": digest(source), "headers": header_hash.hexdigest(),
                  "defines": source_definitions, "tools": tool_key}
        if obj.exists() and stamp.exists() and json.loads(stamp.read_text()) == recipe:
            return obj, None
        if source.suffix == ".asm":
            # Handwritten leaf kernels use the same ISA and ABI as compiled C.
            command = None
        else:
            command = [tools["rcc"], "compile", source, "-o", asm,
                       "--bank-size", BANK_SIZE]
            for path in includes:
                command += ["-I", path]
            for definition in source_definitions:
                command += ["-D", definition]
        print(f"  Ripple: {source.name}", flush=True)
        try:
            if command is not None:
                run(command, out / (stem + ".compile.log"))
            else:
                shutil.copyfile(source, asm)
            run([tools["rasm"], "assemble", asm, "-o", obj, "--bank-size", BANK_SIZE,
                 "--max-immediate", "65535"], out / (stem + ".assemble.log"))
        except RuntimeError as error:
            return None, {"source": str(source), "error": str(error)}
        stamp.write_text(json.dumps(recipe, indent=2) + "\n")
        return obj, None

    pool = ThreadPoolExecutor(max_workers=jobs)
    try:
        # map preserves source/link order while independent compiler processes
        # run concurrently. Each source has its own output and log files.
        for obj, failure in pool.map(compile_source, sources):
            if failure:
                if not keep_going:
                    raise RuntimeError(failure["error"])
                failures.append(failure)
            else:
                objects.append(obj)
    finally:
        pool.shutdown(wait=True, cancel_futures=True)
    (out / "compile-report.json").write_text(json.dumps({"built": len(objects), "failed": failures}, indent=2) + "\n")
    if failures:
        raise RuntimeError(f"{len(failures)} source files failed; see {out / 'compile-report.json'}")
    binary = out / (program + ".bin")
    run([tools["rlink"], ROOT / "runtime/crt0.pobj", ROOT / "runtime/libruntime.par",
         *objects, "-f", "binary", "--bank-size", BANK_SIZE, "-o", binary],
        out / "link.log")
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("program", choices=["numeric", "assets", "libc", "wad_lookup", "graphics_cache", "demo_playback", "stat_storage", "info_storage", "projection_widths", "sky_widths", "event_widths", "texture_columns", "keyboard_controls", "render_kernels", "raster_kernels", "memory_kernels", "engine", "prepare"]
                        + sorted(NATIVE_FIXTURES))
    parser.add_argument("--target", choices=["native", "ripple", "both"], default="both")
    parser.add_argument("--wad", type=Path, default=ROOT / "c-test/examples/doom/doom1.wad")
    parser.add_argument("--keep-going", action="store_true", help="collect all Ripple compilation failures")
    parser.add_argument("--start-map", action="store_true", help="boot directly into E1M1")
    parser.add_argument("--low-detail", action="store_true", help="start with Doom's faster, half-horizontal-resolution world rendering")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1),
                        help="parallel Ripple compiler processes (default: up to 4)")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if args.program in NATIVE_FIXTURES and args.target != "native":
        parser.error(f"{args.program} is a native fixture; use --target native")
    wad = args.wad.resolve()
    if not wad.is_file():
        parser.error(f"WAD not found: {wad}")
    upstream, generated = prepare(wad)
    if args.program == "prepare":
        print(upstream)
        return
    sources = sources_for(args.program, upstream, generated)
    targets = ["native", "ripple"] if args.target == "both" else [args.target]
    for target in targets:
        print(build(args.program, target, sources, upstream, generated, args.keep_going,
                    args.start_map, args.jobs, args.low_detail), flush=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
