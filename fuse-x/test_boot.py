#!/usr/bin/env python3
"""Exercise an actual native kernel against disposable C16 root images."""
import argparse
import hashlib
import shutil
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    args = parser.parse_args()
    executable = args.executable.resolve()
    here = Path(__file__).resolve().parent
    environment = dict(os.environ, RCC_NATIVE_HEADLESS="1", RCC_NATIVE_NET="off",
                       RCC_NATIVE_TIMEOUT="5")

    def run(*arguments, success=True, contains="", launcher=False, cwd=None):
        command = here.parent / "scripts/run-fuse-x.sh" if launcher else executable
        launch_environment = dict(environment, FUSE_X_BUILD=str(executable.parent))
        result = subprocess.run([str(command), *map(str, arguments)],
                                capture_output=True, text=True, env=launch_environment,
                                cwd=cwd, timeout=15)
        output = result.stdout + result.stderr
        assert (result.returncode == 0) == success, output
        assert contains in output, output
        return output

    with tempfile.TemporaryDirectory(prefix="fuse-x-boot-") as temporary:
        root = Path(temporary) / "ROOTFS.IMG"
        subprocess.run(["node", str(here / "image.mjs"), "create", str(root)], check=True)
        original = hashlib.file_digest(root.open('rb'), 'sha256').digest()
        output = run("--boot-check", root, contains="bootstrap check passed")
        assert "Riplix version" in output and "PID 1" in output, output
        banner = (here.parent / "ports/fuzix/banner.tmp.txt").read_text().rstrip("\n")
        assert banner in output.replace("\r", ""), output
        assert "Derived from FUZIX." in output and "Copyright (c) 2014-2026 Alan Cox" in output, output
        assert hashlib.file_digest(root.open('rb'), 'sha256').digest() == original, "read-only boot changed image"
        run("--write-check", root, "hello from the hosted kernel", contains="file contents verified")
        assert hashlib.file_digest(root.open('rb'), 'sha256').digest() != original, "write check did not persist"
        persisted = hashlib.file_digest(root.open('rb'), 'sha256').digest()
        run("--read-check", root, "hello from the hosted kernel", contains="file contents verified")
        assert hashlib.file_digest(root.open('rb'), 'sha256').digest() == persisted, "read check changed image"
        run("--read-check", root, "incorrect contents", success=False, contains="persistent file contents")
        run("--boot-check", Path(temporary) / "missing.img", success=False, contains="cannot open root image")
        invalid = Path(temporary) / "invalid.img"
        invalid.write_bytes(bytes(8192))
        run("--boot-check", invalid, success=False)
        run(success=False, contains="usage:")
        # The launcher must preserve arguments and caller-relative image paths.
        spaced = Path(temporary) / "root with spaces.img"
        shutil.copyfile(root,spaced)
        run("--run-only", "--boot-check", "--disk", spaced.name, launcher=True, cwd=temporary,
            contains="bootstrap check passed")
        run("--run-only", "--disk", spaced, "--write-check", "launcher value with spaces",
            launcher=True, contains="file contents verified")
        written = hashlib.file_digest(spaced.open('rb'),'sha256').digest()
        run("--run-only", "--disk", spaced, "--read-check", "launcher value with spaces",
            launcher=True, contains="file contents verified")
        assert hashlib.file_digest(spaced.open('rb'),'sha256').digest() == written, "launcher read changed image"
        run("--run-only", "--fresh", launcher=True, success=False, contains="cannot be combined")
        run("--timeout", "invalid", launcher=True, success=False, contains="positive whole number")
        run("--disk", launcher=True, success=False, contains="requires a value")
        print("Fuse-X: native boot, clocks, persistence, read-only image, launcher and failure checks passed")


if __name__ == "__main__":
    main()
