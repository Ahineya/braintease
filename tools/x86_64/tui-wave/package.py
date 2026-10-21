#!/usr/bin/env python3
"""Prepare an unpublished RXE1 package subset, using the existing serializer."""
import argparse
import base64
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/packages"))
from catalog import Catalog


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build/x86_64-tui-wave")
    args = parser.parse_args()
    build = args.build.resolve()
    output = build / "catalog"
    output.mkdir(parents=True, exist_ok=True)
    engine = ROOT / "build/pkg/host/pkg-engine"
    if not engine.is_file():
        raise RuntimeError("build the existing ports/pkg host engine first")
    source = """import {commanderFiles,riftFiles} from './rfl/os/files.mjs';
import {unicodeFiles} from './libraries/unicode/files.mjs';
import {kernelCredits} from './ports/fuzix/tools/credits.mjs';
const groups={'commander-data':commanderFiles(),'rift-data':riftFiles(),
 'unicode-data':unicodeFiles(),'system-licenses':kernelCredits()};
for (const [name,files] of Object.entries(groups)) groups[name]=files.filter(f=>f.data&&!f.directory&&f.device===undefined).map(f=>({name:f.name.replace(/^\\//,''),mode:(f.mode??0o100644)&0o777,data:Buffer.from(f.data).toString('base64')}));
console.log(JSON.stringify(groups));"""
    resources = json.loads(subprocess.check_output(["node", "--input-type=module", "-e", source], cwd=ROOT))
    catalog = Catalog(build, output, engine, "x64")
    names = [name for name in ("mux", "rfl-fm", "rift", "keycheck", "levee")
             if (build / "apps" / (name + ".fxe")).is_file()]
    needed = {"system-licenses", "unicode-data"}
    if "rfl-fm" in names:
        needed.add("commander-data")
    if "rift" in names:
        needed.add("rift-data")
    for name in sorted(needed):
        files = [{**f, "data": base64.b64decode(f["data"])} for f in resources[name]]
        catalog.group(name, files, target="any")
    for name in names:
        if name == "levee":
            # Canonical /usr/man remains owned by shared man-pages. Bundle the
            # current override as app-specific documentation without collision.
            payload = (build / "apps/levee.fxe").read_bytes()
            if payload[:12] != b"FUSEX001RXE1":
                raise RuntimeError("Levee input is not native x64 RXE1")
            catalog.package("levee", [
                dict(name="usr/bin/levee", mode=0o755, data=payload),
                dict(name="bin/levee", mode=0o755, data=b'#!/bin/sh\nexec /usr/bin/levee "$@"\n'),
                dict(name="usr/share/doc/levee/levee.1", mode=0o644,
                     data=(ROOT / "ports/fuzix/manual/man1/levee.1").read_bytes())
            ], ["system-licenses@*"])
            catalog.apps.append(name)
            continue
        aliases = {"rfl-fm": ["mc"], "rift": ["vi"]}.get(name, [])
        extra = ["unicode-data@*"] if name in ("rfl-fm", "rift", "keycheck") else []
        catalog.application(name, build / "apps" / (name + ".fxe"), extra=extra, aliases=aliases)
    report = dict(target="riplix-x64-c16", applications=names,
                  aliases={"mc": "rfl-fm", **({"vi": "rift"} if "rift" in names else {})},
                  packages=list(catalog.packages.values()),
                  publication="NOT PUBLISHED; integration owner must review guest acceptance first")
    (output / "catalog.json").write_text(json.dumps(report, indent=2) + "\n")
    with (output / "inspect.log").open("w") as log:
        for package in report["packages"]:
            subprocess.run([engine, "inspect", package["archive"]], stdout=log, stderr=subprocess.STDOUT, check=True)
    print(f"Prepared {len(report['packages'])} unpublished packages", flush=True)


if __name__ == "__main__":
    main()
