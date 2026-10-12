#!/usr/bin/env python3
"""Check native C and existing-ISA memory kernels using the Rust VM only."""
from pathlib import Path
import subprocess
import sys

PORT = Path(__file__).resolve().parent
ROOT = PORT.parent.parent
OUT = PORT / "build/ripple/memory_kernels"
subprocess.run([sys.executable, PORT / "build.py", "memory_kernels", "--target", "both"], check=True)
subprocess.run([PORT / "build/native/memory_kernels/memory_kernels"], check=True)
subprocess.run(["rustc", "--edition=2021", "-O", PORT / "tests/memory_kernels.rs",
                "--extern", f"rvm={ROOT / 'target/release/librvm.rlib'}",
                "-L", f"dependency={ROOT / 'target/release/deps'}",
                "-o", OUT / "verify-kernels"], check=True)
subprocess.run([OUT / "verify-kernels", OUT], check=True)
