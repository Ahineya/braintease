#!/usr/bin/env python3
"""Bounded native/Rust-VM smoke tests for Doom port milestones.

The foundations stage validates real engine arithmetic/tables, the private C
library, and independently decoded WAD assets. The optional engine stage checks
the first title and E1M1 frames against the native build. No Brainfuck runs.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time
from disk_image import prepare_disk

PORT = Path(__file__).resolve().parent
ROOT = PORT.parent.parent
BUILD = PORT / "build"
RVM = ROOT / "target/release/rvm"
BANK_SIZE = 64000
MEMORY_CELLS = 16384000
NUMERIC_OUTPUT = "NUMERIC OK 6e970016 d440d0e1 1c93684a\n"
LIBC_OUTPUT = "LIBC validation_ok 1\n"
WAD_LOOKUP_OUTPUT = "WAD LOOKUP OK 1264 lumps, 350 names, 598 references, final block\n"
GRAPHICS_OUTPUT = "GRAPHICS OK decoded widths 1/9, bank-crossing offsets, cache purge/reload, 12 malformed cases\n"
DEMO_OUTPUT = "DEMO OK signed octets, bounded records, DEMO1 5026 9ca0bb8e\n"
STAT_OUTPUT = "STAT STORAGE OK 32 retained snapshots, lazy allocation, wide times\n"
INFO_OUTPUT = "INFO OK 967 states, 137 objects, canonical fields and callbacks\n"
PROJECTION_OUTPUT = "PROJECTION OK wide wall tiers, empty spans and plane clipping\n"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class Smoke:
    def __init__(self, args):
        self.args = args
        self.out = args.artifacts.resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        self.wad = args.wad.resolve()
        require(self.wad.is_file(), f"WAD fixture not found: {self.wad}")
        require(RVM.is_file(), f"Rust VM not built: {RVM}")
        self.wad_hash = digest(self.wad)
        self.disk = self.out / "fixture-copy.wad"
        require(self.disk != self.wad, "artifact disk must differ from the source WAD")
        disk_metadata = prepare_disk(self.wad, self.disk)
        self.disk_hash = disk_metadata["sha256"]
        self.result = {
            "format_version": 1,
            "stage": args.stage,
            "success": False,
            "wad": {"source": str(self.wad), "copy": str(self.disk),
                    "octets": self.wad.stat().st_size, "sha256": self.wad_hash},
            "disk_image": disk_metadata,
            "limits": {"instructions": args.max_instructions, "seconds": args.timeout,
                       "build_seconds": args.build_timeout},
            "vm": {"sha256": digest(RVM), "bank_size": BANK_SIZE,
                   "memory_cells": MEMORY_CELLS, "seed": 1, "clock": "unthrottled"},
            "commands": [],
            "checks": [],
        }

    def run(self, label, command, timeout=None):
        command = [str(part) for part in command]
        stdout = self.out / (label + ".stdout.log")
        stderr = self.out / (label + ".stderr.log")
        record = {"label": label, "command": command, "stdout": str(stdout), "stderr": str(stderr)}
        self.result["commands"].append(record)
        started = time.monotonic()
        with stdout.open("w") as output, stderr.open("w") as errors:
            try:
                process = subprocess.run(command, cwd=ROOT, stdout=output, stderr=errors,
                                         timeout=timeout or self.args.timeout)
                record["exit_code"] = process.returncode
            except subprocess.TimeoutExpired:
                record["timed_out"] = True
                raise RuntimeError(f"{label} exceeded its wall-time limit; see {stdout} and {stderr}")
            finally:
                record["seconds"] = round(time.monotonic() - started, 3)
        require(process.returncode == 0,
                f"{label} failed with exit {process.returncode}; see {stdout} and {stderr}")
        return stdout.read_text()

    def build(self, program, start_map=False, target="both"):
        print(f"Building {program}{' (E1M1)' if start_map else ''} ({target})", flush=True)
        command = [sys.executable, PORT / "build.py", program, "--target", target, "--wad", self.wad]
        if start_map:
            command.append("--start-map")
        self.run("build-" + program + ("-map" if start_map else ""), command, self.args.build_timeout)
        self.result["build_manifest"] = json.loads((BUILD / "manifest.json").read_text())

    def vm(self, label, program, expected_raw=None, frame=1, input_script=None):
        binary = BUILD / "ripple" / program / (program + ".bin")
        report = self.out / (label + ".report.json")
        report.unlink(missing_ok=True)
        command = [RVM, binary, "--bank-size", BANK_SIZE, "--memory", MEMORY_CELLS,
                   "--headless", "--clock", "unthrottled", "--seed", 1, "--disk", self.disk,
                   "--exit-code", "--max-instructions", self.args.max_instructions,
                   "--timeout", self.args.timeout, "--report", report]
        if expected_raw:
            raw = self.out / (label + ".rgb565")
            png = self.out / (label + ".png")
            raw.unlink(missing_ok=True)
            png.unlink(missing_ok=True)
            command += ["--screenshot", png, "--raw-frame", raw,
                        "--capture-frame", frame, "--expect-frame", expected_raw]
        if input_script:
            command += ["--input-script", input_script]
        stdout = self.run(label, command, self.args.timeout + 10)
        require(report.is_file(), f"{label}: VM did not write a report")
        metadata = json.loads(report.read_text())
        require(metadata.get("success") is True, f"{label}: unsuccessful VM report")
        expected_stop = "captured_frame" if expected_raw else "halted"
        require(metadata.get("stop_reason") == expected_stop,
                f"{label}: expected {expected_stop}, got {metadata.get('stop_reason')}")
        for name, expected in (("bank_size", BANK_SIZE), ("memory_cells", MEMORY_CELLS),
                               ("seed", 1), ("disk_sha256", self.disk_hash),
                               ("tick_hz", 35), ("clock", "unthrottled"),
                               ("binary_sha256", digest(binary)), ("input_overflow", False)):
            require(metadata.get(name) == expected, f"{label}: report {name} differs from requested run")
        if expected_raw:
            require((metadata.get("width"), metadata.get("height"), metadata.get("frame")) == (320, 200, frame),
                    f"{label}: wrong capture dimensions or frame number")
            require(raw.is_file() and raw.read_bytes() == expected_raw.read_bytes(),
                    f"{label}: captured RGB565 differs from reference")
        if input_script:
            require(metadata.get("input_script_sha256") == digest(input_script),
                    f"{label}: replay script identity differs")
        self.result["checks"].append({"label": label, "vm_report": metadata})
        require(digest(self.disk) == self.disk_hash, f"{label}: guest modified the WAD disk image")
        return stdout

    def foundations(self):
        for program, expected in (("numeric", NUMERIC_OUTPUT), ("libc", LIBC_OUTPUT),
                                  ("wad_lookup", WAD_LOOKUP_OUTPUT),
                                  ("graphics_cache", GRAPHICS_OUTPUT), ("demo_playback", DEMO_OUTPUT),
                                  ("stat_storage", STAT_OUTPUT), ("info_storage", INFO_OUTPUT),
                                  ("projection_widths", PROJECTION_OUTPUT)):
            self.build(program)
            native = self.run(program + "-native", [BUILD / "native" / program / program])
            require(native == expected, f"{program}: native diagnostic output differs from expected text")
            ripple = self.vm(program + "-ripple", program)
            require(ripple == expected, f"{program}: Ripple diagnostic output differs from expected text")
            self.result["checks"].append({"label": program + "-exact-output", "expected": expected})
            print(f"PASS {program}: native and Rust VM", flush=True)

        self.build("assets")
        oracle_raw = self.out / "assets-oracle.rgb565"
        oracle_manifest = self.out / "assets-oracle.json"
        oracle = [sys.executable, PORT / "tools/check_assets.py", self.wad]
        self.run("assets-oracle", oracle + ["--self-test", "--write-raw", oracle_raw,
                                           "--manifest", oracle_manifest])
        native_raw = self.out / "assets-native.rgb565"
        native_raw.unlink(missing_ok=True)
        self.run("assets-native", [BUILD / "native/assets/assets", self.wad, native_raw])
        self.run("assets-native-check", oracle + ["--check-raw", native_raw, "--check-log",
                                                  self.out / "assets-native.stdout.log"])
        self.vm("assets-ripple", "assets", oracle_raw)
        self.run("assets-ripple-check", oracle + ["--check-raw", self.out / "assets-ripple.rgb565",
                                                  "--check-log", self.out / "assets-ripple.stdout.log"])
        self.result["checks"].append({"label": "assets-independent-oracle",
                                      "oracle": json.loads(oracle_manifest.read_text())})
        print("PASS assets: malformed-input checks, E1M1 geometry, texture references and exact TITLEPIC pixels", flush=True)

    def portability(self):
        # These fixtures include engine internals and intentionally use the
        # native linker's dead-section elimination and assertion facilities.
        # Target behavior is checked separately by the full engine comparison.
        for program in ("map_decode", "world_widths", "clock_widths"):
            self.build(program, target="native")
            command = [BUILD / "native" / program / program]
            if program == "map_decode":
                command.append(self.wad)
            output = self.run(program + "-native", command)
            self.result["checks"].append({"label": program + "-native", "output": output})
            print("PASS " + output.strip(), flush=True)

    def engine_case(self, name, mode, frame, expected_state, input_script=None):
        label = "engine-" + name
        native_raw = self.out / (label + "-native.rgb565")
        native_raw.unlink(missing_ok=True)
        command = [BUILD / "native/engine/engine", native_raw, frame, mode]
        if input_script:
            events = json.loads(input_script.read_text())
            native_script = self.out / (label + ".input.txt")
            native_script.write_text("".join(
                f"{event['tick']} {event['key']} {int(event['down'])}\n" for event in events))
            command.append(native_script)
        native = self.run(label + "-native", command)
        require(native_raw.is_file() and native_raw.stat().st_size == 128000,
                f"{label}: native capture is missing or has wrong dimensions")
        ripple = self.vm(label + "-ripple", "engine", native_raw, frame, input_script)
        pattern = r"^ENGINE frame (\d+) tick (\d+) state (\d+) episode (\d+) map (\d+)$"
        native_frames = re.findall(pattern, native, re.MULTILINE)
        ripple_frames = re.findall(pattern, ripple, re.MULTILINE)
        require(native_frames and native_frames == ripple_frames,
                f"{label}: native and Ripple frame/tick/state logs differ")
        final = tuple(map(int, native_frames[-1]))
        report = self.result["checks"][-1]["vm_report"]
        require(report["tick"] == final[1], f"{label}: VM and engine logical ticks differ")
        require(final[0] == frame and final[2] == expected_state,
                f"{label}: capture did not reach requested frame/state")
        if expected_state == 0:
            require(final[3:] == (1, 1), f"{label}: capture did not reach E1M1")
        if name == "menu-start":
            require([(int(row[0]), int(row[1]), int(row[2])) for row in native_frames]
                    == [(n, n + 1, 3 if n < 8 else 0) for n in range(1, 11)],
                    f"{label}: menu replay did not enter the world at tick 9")
        for output in (native, ripple):
            require("DOOM PORT WAD " + self.wad_hash in output, f"{label}: WAD identity missing")
            require("DOOM ERROR:" not in output, f"{label}: engine reported an error")
        self.result["checks"].append({"label": label + "-native-vs-ripple",
                                      "frames": native_frames, "raw_sha256": digest(native_raw),
                                      "native_binary_sha256": digest(BUILD / "native/engine/engine")})
        print(f"PASS engine {name}: exact native/Rust-VM pixels and frame/tick/state logs", flush=True)

    def engine(self):
        # The target startup state is selected at build time: no guest argv ABI
        # is required. The menu fixture then enters E1M1 through real key events.
        self.build("engine")
        self.engine_case("title", "title", self.args.engine_frame, 3)
        self.engine_case("menu-start", "title", 10, 0, PORT / "tests/menu-start.json")
        self.build("engine", start_map=True)
        self.engine_case("map", "map", self.args.engine_frame, 0)

    def finish(self):
        require(digest(self.wad) == self.wad_hash, "source WAD changed during testing")
        require(digest(self.disk) == self.disk_hash, "WAD disk image changed during testing")
        self.result["success"] = True

    def save(self):
        path = self.out / "results.json"
        path.write_text(json.dumps(self.result, indent=2, sort_keys=True) + "\n")
        return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", choices=("foundations", "portability", "engine"), default="foundations",
                        help="run one milestone's checks; engine needs a linked full engine")
    parser.add_argument("--wad", type=Path, default=ROOT / "c-test/examples/doom/doom1.wad")
    parser.add_argument("--artifacts", type=Path, default=BUILD / "test-artifacts")
    parser.add_argument("--max-instructions", type=int, default=2000000000)
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--build-timeout", type=int, default=600)
    parser.add_argument("--engine-frame", type=int, default=1)
    args = parser.parse_args()
    if min(args.max_instructions, args.timeout, args.build_timeout, args.engine_frame) < 1:
        parser.error("limits and frame numbers must be positive")
    smoke = None
    try:
        smoke = Smoke(args)
        getattr(smoke, args.stage)()
        smoke.finish()
    except (OSError, RuntimeError, ValueError) as error:
        if smoke:
            smoke.result["error"] = str(error)
            print(f"FAIL: {error}\nResults: {smoke.save()}", file=sys.stderr)
        else:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(f"All {args.stage} checks passed. Results: {smoke.save()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
