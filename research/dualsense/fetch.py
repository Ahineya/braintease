#!/usr/bin/env python3
"""Collect or verify pinned firmware files. No USB/HID or flashing operations."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import tempfile
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parent


def digest(data):
    return hashlib.sha256(data).hexdigest()


def catalog(root=ROOT):
    sources = json.loads((root / "sources.json").read_text())
    raw = (root / "catalog.json").read_bytes()
    if digest(raw) != sources["catalog_sha256"]:
        raise ValueError("catalog hash differs from pinned sources.json")
    metadata = json.loads(raw)
    if metadata["schemaVersion"] != 1:
        raise ValueError("unsupported catalog version")
    entries = metadata["firmwares"]
    seen = set()
    for entry in entries:
        path = PurePosixPath(entry["file"])
        if path.is_absolute() or ".." in path.parts or path.parts[0] != "firmware":
            raise ValueError("unsafe firmware path")
        if str(path) in seen:
            raise ValueError("duplicate firmware path")
        seen.add(str(path))
        version, updater = entry["version"], entry["updaterId"]
        if not re.fullmatch(r"0x[0-9a-fA-F]{4}", version):
            raise ValueError("invalid version")
        if updater not in {"FWUPDATE0004", "FWUPDATE000B", "FWUPDATE000E", "FWUPDATE0044"}:
            raise ValueError("unexpected updater family")
        expected = ("https://fwupdater.dl.playstation.net/fwupdater/fwupdate"
                    + updater[-4:] + "/" + version + "/" + updater + ".bin")
        if entry["sourceUrl"] != expected or path.name != updater + ".bin":
            raise ValueError("unexpected firmware URL or filename")
        if not 256 <= entry["size"] <= 4 * 1024 * 1024:
            raise ValueError("unexpected firmware size")
        if not re.fullmatch(r"[0-9a-fA-F]{64}", entry["sha256"]):
            raise ValueError("invalid firmware hash")
    return sources, entries


def verify(data, entry):
    if len(data) != entry["size"]:
        raise ValueError(f"size mismatch: {entry['file']}: {len(data)}")
    if digest(data) != entry["sha256"].lower():
        raise ValueError(f"SHA256 mismatch: {entry['file']}")


def acquire(root, sources, entry, offline):
    path = root / entry["file"]
    if path.exists():
        verify(path.read_bytes(), entry)
        return entry["file"], "verified existing file", None
    if offline:
        raise FileNotFoundError(path)
    official = entry["sourceUrl"]
    mirror = ("https://raw.githubusercontent.com/Paliverse/DualSense-List-of-Firmwares/"
              + sources["archive_commit"] + "/" + entry["file"])
    failures = []
    for url in [official, mirror]:
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "DualSenseOfflineResearch/1"})
            with urllib.request.urlopen(request, timeout=30) as response:
                final_url = response.geturl()
                data = response.read(entry["size"] + 1)
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            failures.append(f"{url}: {exc}")
            continue
        # A received but incorrect payload is an error, not a reason to hide it
        # behind another source. Only network failures permit mirror fallback.
        verify(data, entry)
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
        try:
            # Publish the complete file atomically without replacing originals.
            temporary.chmod(0o444)
            os.link(temporary, path)
        finally:
            temporary.unlink()
        return entry["file"], final_url, failures
    raise RuntimeError("firmware download failed:\n" + "\n".join(failures))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-offline", action="store_true")
    parser.add_argument("--jobs", type=int, default=4, choices=range(1, 9))
    args = parser.parse_args()
    sources, entries = catalog()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(acquire, ROOT, sources, e, args.verify_offline) for e in entries]
        records = []
        for future in futures:
            name, origin, failures = future.result()
            print(name, origin, flush=True)
            records.append({"file": name, "origin": origin, "network_failures": failures})
    if not args.verify_offline:
        out = ROOT / "analysis"
        out.mkdir(exist_ok=True)
        path = out / "downloads.json"
        # Retain the first actual download provenance on an incremental rerun.
        previous = {x["file"]: x for x in json.loads(path.read_text())} if path.exists() else {}
        for row in records:
            if row["origin"] != "verified existing file" or row["file"] not in previous:
                previous[row["file"]] = row
        path.write_text(json.dumps(list(previous.values()), indent=2) + "\n")
    print(f"Verified {len(entries)} images, {sum(e['size'] for e in entries)} bytes")


if __name__ == "__main__":
    main()
