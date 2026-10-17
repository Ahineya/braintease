#!/usr/bin/env python3
"""Exercise an actual native kernel against disposable C16 root images."""
import argparse
import hashlib
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
        original = hashlib.sha256(root.read_bytes()).digest()
        output = run("--boot-check", root, contains="bootstrap check passed")
        assert "FUZIX version" in output and "PID 1" in output, output
        assert hashlib.sha256(root.read_bytes()).digest() == original, "read-only boot changed image"
        run("--write-check", root, "hello from the hosted kernel", contains="file contents verified")
        assert hashlib.sha256(root.read_bytes()).digest() != original, "write check did not persist"
        persisted = hashlib.sha256(root.read_bytes()).digest()
        run("--read-check", root, "hello from the hosted kernel", contains="file contents verified")
        assert hashlib.sha256(root.read_bytes()).digest() == persisted, "read check changed image"
        run("--read-check", root, "incorrect contents", success=False, contains="persistent file contents")
        run("--boot-check", Path(temporary) / "missing.img", success=False, contains="cannot open root image")
        invalid = Path(temporary) / "invalid.img"
        invalid.write_bytes(bytes(8192))
        run("--boot-check", invalid, success=False)
        run(success=False, contains="usage:")
        # The launcher must preserve arguments and caller-relative image paths.
        spaced = Path(temporary) / "root with spaces.img"
        spaced.write_bytes(root.read_bytes())
        run("--run-only", "--boot-check", "--disk", spaced.name, launcher=True, cwd=temporary,
            contains="bootstrap check passed")
        run("--run-only", "--disk", spaced, "--write-check", "launcher value with spaces",
            launcher=True, contains="file contents verified")
        written = spaced.read_bytes()
        run("--run-only", "--disk", spaced, "--read-check", "launcher value with spaces",
            launcher=True, contains="file contents verified")
        assert spaced.read_bytes() == written, "launcher read changed image"
        run("--run-only", "--fresh", launcher=True, success=False, contains="cannot be combined")
        run("--timeout", "invalid", launcher=True, success=False, contains="positive whole number")
        run("--disk", launcher=True, success=False, contains="requires a value")
        print("Fuse-X: native boot, clocks, persistence, read-only image, launcher and failure checks passed")


if __name__ == "__main__":
    main()
