#!/usr/bin/env python3
"""Fetch the locked Doom sources without changing existing checkouts.

Use --check for offline verification. Port patches and adapters belong outside
these ignored directories; this script refuses dirty or mismatched checkouts.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
LOCK = ROOT / "vendor" / "doom-sources.json"


def git(directory, *args):
    return subprocess.check_output(
        ["git", "-C", str(directory), *args], text=True, stderr=subprocess.PIPE
    ).strip()


def verify(source, destination):
    if not (destination / ".git").is_dir():
        raise ValueError(f"{destination}: expected an independent Git checkout")
    if git(destination, "remote", "get-url", "origin") != source["url"]:
        raise ValueError(f"{destination}: origin differs from the source lock")
    actual = git(destination, "rev-parse", "HEAD")
    if actual != source["revision"]:
        raise ValueError(f"{destination}: expected {source['revision']}, found {actual}")
    if git(destination, "status", "--porcelain", "--untracked-files=all"):
        raise ValueError(f"{destination}: local changes found; preserve them outside vendor first")
    if not (destination / source["license_file"]).is_file():
        raise ValueError(f"{destination}: upstream license file is missing")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify only; no downloads or writes")
    args = parser.parse_args()
    lock = json.loads(LOCK.read_text())
    if lock["version"] != 1:
        raise ValueError("Unsupported Doom source lock version")
    for source in lock["sources"]:
        if not re.fullmatch(r"[0-9a-f]{40}", source["revision"]):
            raise ValueError("Source revisions must be full Git commit hashes")
        relative = Path(source["path"])
        if relative.is_absolute() or len(relative.parts) != 2 or relative.parts[0] != "vendor":
            raise ValueError("Source paths must be direct children of vendor/")
        if relative.name in (".", ".."):
            raise ValueError("Invalid source checkout path")
        destination = ROOT / relative
        if destination.is_symlink():
            raise ValueError(f"{destination}: refusing a symlink checkout")
        if not destination.exists():
            if args.check:
                raise ValueError(f"Missing {destination}; run scripts/fetch-doom-sources.py")
            destination.parent.mkdir(parents=True, exist_ok=True)
            # Fetch a specific commit, never whatever a branch points at today.
            # Publish the checkout only after verification; a failed download
            # cannot leave a half-populated destination for the next run.
            with tempfile.TemporaryDirectory(prefix=".doom-fetch-", dir=destination.parent) as temp:
                checkout = Path(temp) / "checkout"
                subprocess.run(["git", "init", "--quiet", str(checkout)], check=True)
                git(checkout, "remote", "add", "origin", source["url"])
                subprocess.run(
                    ["git", "-C", str(checkout), "fetch", "--depth=1", "--no-tags",
                     "origin", source["revision"]], check=True
                )
                git(checkout, "checkout", "--quiet", "--detach", source["revision"])
                verify(source, checkout)
                checkout.rename(destination)
        verify(source, destination)
        print(f"OK {source['name']}: {source['revision']} ({source['path']})")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr.strip(), file=sys.stderr)
        sys.exit(1)
