#!/usr/bin/env python3
"""Build RFL applications against the shared Riplix/Fuse-X guest libc."""
import argparse
import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "ports/fuzix"


def run(arguments, **kwargs):
    result = subprocess.run(list(map(str, arguments)), capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace") + result.stdout.decode(errors="replace"))
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--os", choices=("fuzix", "fuse-x", "macos"), required=True)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--rfl", type=Path, default=ROOT / "target/release/rfl")
    parser.add_argument("--rcc", type=Path, default=ROOT / "target/release/rcc")
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if source == output:
        raise RuntimeError("output must not overwrite the RFL source")
    if args.os == "macos":
        from build_host import build_host
        build_host(source, output, args.rfl.resolve(), args.rcc.resolve(), args.build_dir)
        return
    build = (args.build_dir or ROOT / ("build/fuzix" if args.os == "fuzix" else "fuse-x/build")).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    # Keep a failed compilation from replacing a previously working executable.
    with tempfile.TemporaryDirectory(prefix=".rfl-build-", dir=output.parent) as temporary:
        work = Path(temporary)
        ir = work / "program.ir"
        ir.write_bytes(run([args.rfl.resolve(), "compile", source, "--emit-ir", "--target",
                            "ripple" if args.os == "fuzix" else "native"]))
        if re.search(r"@(mmio_read|mmio_write|rfl_host_clock_read|rfl_host_idle|rcc_hosted_input)\(", ir.read_text()):
            raise RuntimeError("standalone MMIO/host/network libraries cannot run in an OS process; use std/posix and std/curses guest services")
        bridge = ROOT / "rfl/os/posix.c"
        curses = "@rfl_curses_" in ir.read_text()
        unicode = "@ru_" in ir.read_text()
        bridges = [bridge] + ([ROOT / "rfl/os/curses.c"] if curses else [])
        if "@rfl_sftp_" in ir.read_text():
            bridges.append(ROOT / "ports/ssh/commander.c")
        telegram = "@tg_client_" in ir.read_text()
        if "@music_new" in ir.read_text():
            bridges.extend(ROOT / "ports/music" / name for name in ("wav.c", "music.c"))
        if re.search(r"@rfl_(socket|poll)_", ir.read_text()) or telegram:
            bridges.extend(ROOT / "rfl/os" / name for name in ("socket.c", "poll.c", "dns.c"))
        if "@rfl_crypto_" in ir.read_text() or telegram:
            bridges.append(ROOT / "rfl/os/crypto.c")
        if telegram:
            bridges.extend(ROOT / "ports/telegram" / (name + ".c") for name in
                           ("wire", "hash", "inflate", "tg_crypto", "keys", "packet", "storage", "client", "handshake", "account", "receive", "tl", "tl_schema", "model", "dialogs", "updates"))
        if "@rfl_text_paint" in ir.read_text():
            bridges.append(ROOT / "rfl/os/text_curses.c")
        theme = "@tui_theme_" in ir.read_text()
        if theme:
            bridges.append(ROOT / "libraries/tui-theme/tui_theme.c")
        artifact = work / output.name
        if args.os == "fuse-x":
            run([sys.executable, ROOT / "fuse-x/cc.py", ir, *bridges, "--compiler", args.rcc.resolve(),
                 "--build", build, "-o", artifact, *(["--curses"] if curses else [])])
        else:
            run(["make", "--no-print-directory", "-C", PORT, f"BUILD={build}",
                 f"RCC={args.rcc.resolve()}", f"RFL_CURSES={int(curses)}", f"RFL_UNICODE={int(unicode)}", "rfl-sdk"])
            sdk = json.loads(run(["make", "-s", "--no-print-directory", "-C", PORT,
                                  f"BUILD={build}", f"RFL_CURSES={int(curses)}", f"RFL_UNICODE={int(unicode)}", "rfl-sdk-info"]))
            flags = ["--bank-size", "64000", "-DRIPPLE_MMU=1", "-DNO_64BIT"]
            for directory in [PORT / "ripple/include", PORT / "upstream/Library/include",
                              ROOT / "runtime/include", PORT / "ripple"]:
                flags += ["-I", str(directory)]
            objects = []
            for name, input_file in [("program", ir), *[(p.stem, p) for p in bridges]]:
                assembly, obj = work / f"{name}.asm", work / f"{name}.pobj"
                run([args.rcc.resolve(), "compile", input_file, *flags, "-o", assembly])
                run([ROOT / "src/ripple-asm/target/release/rasm", "assemble", assembly, "-o", obj,
                     "--bank-size", "64000", "--max-immediate", "65535"])
                objects.append(obj)
            run([sys.executable, ROOT / "ports/fuzix/tools/shared-link.py", "--build", build, "--compiler", args.rcc.resolve(), "--", sdk["entry"], *objects,
                 *sdk["libraries"], "-f", "rxe", "--bank-size", "64000", "-o", artifact])
        artifact.replace(output)
    system = "Riplix" if args.os == "fuzix" else "Fuse-X"
    print(f"Built {system} application: {output}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(f"RFL application build: {error}", file=sys.stderr)
        sys.exit(1)
