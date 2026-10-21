#!/usr/bin/env python3
"""Check the unpublished package transaction and exact payloads on a host root.

This complements QEMU execution; native x64 programs are not run on the host.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def sha(path):
    return hashlib.file_digest(path.open("rb"), "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build/x86_64-tui-wave")
    args = parser.parse_args()
    build = args.build.resolve()
    catalog = json.loads((build / "catalog/catalog.json").read_text())
    engine = ROOT / "build/pkg/host/pkg-engine"
    with tempfile.TemporaryDirectory(prefix="tui-pkg-", dir=build) as temporary:
        stage = Path(temporary)
        (stage / "etc").mkdir()
        preserved = {"etc/mc.ini": b"custom commander setting\n", "etc/rift.ini": b"custom editor setting\n"}
        for name, data in preserved.items():
            (stage / name).write_bytes(data)
        command = [str(engine), "--root", str(stage), "--target", "riplix-x64-c16"]
        with (build / "package-transaction.log").open("w") as log:
            for package in catalog["packages"]:
                subprocess.run([*command, "install", package["archive"]], stdout=log, stderr=subprocess.STDOUT, check=True)
                for name in package["files"]:
                    if not (stage / name).is_file():
                        raise RuntimeError("missing installed path " + name)
            for name in catalog["applications"]:
                if sha(stage / "usr/bin" / name) != sha(build / "apps" / (name + ".fxe")):
                    raise RuntimeError("installed payload differs: " + name)
            for alias, name in catalog["aliases"].items():
                if sha(stage / "usr/bin" / alias) != sha(stage / "usr/bin" / name):
                    raise RuntimeError("alias payload differs: " + alias)
            if "levee" in catalog["applications"]:
                if sha(stage / "usr/share/doc/levee/levee.1") != sha(ROOT / "ports/fuzix/manual/man1/levee.1"):
                    raise RuntimeError("Levee keyboard documentation differs from current override")
                levee = next(p for p in catalog["packages"] if p["name"] == "levee")
                if any(name.startswith("usr/man/") for name in levee["files"]):
                    raise RuntimeError("Levee must not claim shared man-pages paths")
            for name, data in preserved.items():
                if (stage / name).read_bytes() != data:
                    raise RuntimeError("user configuration changed: " + name)
            subprocess.run([*command, "list"], stdout=log, stderr=subprocess.STDOUT, check=True)
            for package in reversed(catalog["packages"]):
                subprocess.run([*command, "remove", package["name"]], stdout=log, stderr=subprocess.STDOUT, check=True)
            for name, data in preserved.items():
                if (stage / name).read_bytes() != data:
                    raise RuntimeError("uninstall changed user configuration: " + name)
            for name in catalog["applications"]:
                if (stage / "usr/bin" / name).exists():
                    raise RuntimeError("uninstall retained executable: " + name)
    result = dict(status="passed", packages=len(catalog["packages"]),
                  installed_payloads="byte-identical RXE1 files and aliases",
                  user_configuration="preserved through install and uninstall",
                  archives={p["name"]: p["sha256"] for p in catalog["packages"]},
                  guest_execution=False, publication=False)
    (build / "package-transaction.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)


if __name__ == "__main__":
    main()
