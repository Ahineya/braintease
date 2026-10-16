#!/usr/bin/env python3
"""Exercise rOS dependency tracking with real tools and isolated source copies.

Build the release RCC, RVM, RASM and RLINK tools before running this test.
"""
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class IncrementalBuild(unittest.TestCase):
    def test_run_only_and_jit_selection(self):
        with tempfile.TemporaryDirectory(prefix="ros-launcher-") as temporary:
            work = Path(temporary)
            tools = work / "tools"
            tools.mkdir()
            for name in ("cargo", "make", "node"):
                tool = tools / name
                tool.write_text("#!/bin/sh\necho unexpected-build-tool >&2\nexit 99\n")
                tool.chmod(0o755)
            # A finite hot loop verifies actual JIT execution through the
            # launcher, without spending time booting a complete OS per case.
            code = [(0x0E, 15, 20000, 0), (0x0A, 15, 15, 0xFFFF),
                    (0x16, 15, 0, 0xFFFF), (0, 0, 0, 0)]
            binary = b"RLINK" + struct.pack("<HII", 64, 0, len(code))
            binary += b"".join(struct.pack("<BBHHH", op, op, a, b, c) for op, a, b, c in code)
            binary += struct.pack("<I", 0)
            (work / "bios.bin").write_bytes(binary)
            (work / "ros.img").write_bytes(bytes(512))
            before = {p: p.stat().st_mtime_ns for p in work.iterdir() if p.is_file()}
            env = dict(os.environ, ROS_BUILD=str(work), RIPPLE_NET="off",
                       PATH=str(tools) + os.pathsep + os.environ["PATH"])
            for index, flags in enumerate(([], ["--jit"], ["--no-jit"], ["--", "--jit"])):
                with self.subTest(flags=flags):
                    report = work / f"report-{index}.json"
                    result = subprocess.run([
                        str(ROOT / "scripts/run-ros.sh"), "--run-only", "--headless",
                        "--memory", "4096", "--timeout", "5", "--report", str(report), *flags,
                    ], env=env, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    data = json.loads(report.read_text())
                    self.assertEqual(data["stop_reason"], "halted")
                    if flags == ["--no-jit"]:
                        self.assertIsNone(data.get("jit"))
                    else:
                        self.assertGreater(data["jit"]["native_instructions"], 0)
                    self.assertEqual({p: p.stat().st_mtime_ns for p in before}, before)
            # No forwarded VM options also works with macOS's Bash 3.2,
            # whose nounset mode rejects an unguarded empty-array expansion.
            result = subprocess.run([str(ROOT / "scripts/run-ros.sh"), "--run-only"],
                                    env=env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            for flags in (["--run-only", "--fresh"], ["--run-only", "--build-only"]):
                result = subprocess.run([str(ROOT / "scripts/run-ros.sh"), *flags],
                                        env=env, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("cannot be combined", result.stderr)
            missing = work / "missing"
            result = subprocess.run([str(ROOT / "scripts/run-ros.sh"), "--run-only"],
                                    env=dict(env, ROS_BUILD=str(missing)),
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("without --run-only to build", result.stderr)
            self.assertFalse(missing.exists())

    def test_image_dependencies(self):
        with tempfile.TemporaryDirectory(prefix="ros-build-") as temporary:
            work = Path(temporary)
            source = work / "examples/rOS"
            runtime = work / "runtime"
            build = work / "build"
            shutil.copytree(ROOT / "c-test/examples/rOS", source)
            for name in ("brainfuck.c", "sierpinski.c"):
                shutil.copy2(ROOT / "c-test/examples" / name, source.parent / name)
            shutil.copytree(ROOT / "runtime", runtime, ignore=shutil.ignore_patterns(
                "*.asm", "*.pobj", "*.par", "*.bf", "*.bfm", ".bank-size"))
            for name in ("crt0.asm", "ros/crt0_ros.asm", "ros/crt0_sys.asm", "ros/int21.asm"):
                shutil.copy2(ROOT / "runtime" / name, runtime / name)
            # A local wrapper lets us change compiler freshness without touching
            # the installed compiler or any source in the working checkout.
            compiler = work / "rcc"
            compiler.write_text("#!/bin/sh\nexec " + shlex.quote(str(ROOT / "target/release/rcc")) + ' "$@"\n')
            compiler.chmod(0o755)

            def make(*settings):
                result = subprocess.run([
                    "make", "--no-print-directory", "-C", str(source), "images",
                    f"ROOT={ROOT}", f"BUILD={build}", f"RUNTIME={runtime}",
                    f"RCC={compiler}", *settings,
                ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                self.assertEqual(result.returncode, 0, result.stdout[-8000:])

            def snapshot():
                return {p: p.stat().st_mtime_ns for root in (build, runtime)
                        for p in root.rglob("*") if p.is_file() and
                        (p.suffix in (".asm", ".pobj", ".par", ".bin", ".SYS", ".COM", ".RXE")
                         or p.name in (".config", ".bank-size"))}

            def changed(before):
                return {p for p, stamp in snapshot().items() if before.get(p) != stamp}

            def advance():
                # Also work with Make/filesystems that compare whole seconds.
                time.sleep(1.05)

            make()
            before = snapshot()
            make()
            self.assertEqual(changed(before), set(), "unchanged builds must not rewrite artifacts")

            advance()
            hello = source / "hello.c"
            hello.write_text(hello.read_text().replace("Hello from RXE", "Hello from the rebuilt RXE"))
            make()
            self.assertEqual(changed(before), {build / "hello.asm", build / "hello.pobj", build / "HELLO.RXE"})

            advance()
            before = snapshot()
            header = source / "imem.h"
            header.write_text(header.read_text() + "\n/* dependency check */\n")
            make()
            self.assertIn(build / "loader.asm", changed(before))
            self.assertIn(build / "KERNEL.SYS", changed(before))

            advance()
            before = snapshot()
            os.utime(runtime / "putchar.pobj", None)
            make()
            self.assertIn(build / "bios.bin", changed(before), "runtime input changes must relink BIOS")
            self.assertNotIn(build / "COMMAND.COM", changed(before), "unrelated images must stay intact")
            self.assertNotIn(build / "bios.asm", changed(before), "link inputs do not require C recompilation")

            advance()
            before = snapshot()
            os.utime(compiler, None)
            make()
            self.assertIn(build / "hello.asm", changed(before))
            self.assertIn(runtime / "puts.asm", changed(before))

            advance()
            before = snapshot()
            make("BANK_SIZE=64001")
            self.assertIn(build / "hello.asm", changed(before))
            self.assertIn(runtime / "puts.asm", changed(before))
            self.assertTrue((build / ".config").read_text().startswith("64001:"))
            before = snapshot()
            make("BANK_SIZE=64001")
            self.assertEqual(changed(before), set(), "unchanged settings must stay stable")

            (runtime / "putchar.pobj").unlink()
            make("BANK_SIZE=64001")
            self.assertTrue((runtime / "putchar.pobj").is_file(), "missing runtime objects must be rebuilt")


if __name__ == "__main__":
    unittest.main()
