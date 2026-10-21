#!/usr/bin/env python3
"""Exercise TUI candidates through real PS/2/TTY input in disposable QEMU.

Uses the production EFI and captured geometry, with fake GPT payloads and a
private sparse root clone. It never opens an HP disk or changes boot files.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/x86_64"))
from boot import executable, firmware
from gpt_image import load_capture, write_image
from qemu_check import run_shell_check


def sha(path):
    return hashlib.file_digest(path.open("rb"), "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot", type=Path, default=ROOT / "build/x86_64-radio-console/normal")
    parser.add_argument("--build", type=Path, default=ROOT / "build/x86_64-tui-wave")
    parser.add_argument("--timeout", type=int, default=900)
    mode_group = parser.add_mutually_exclusive_group()
    mode_group.add_argument("--packages", action="store_true", help="install the unpublished local RPK subset inside the private guest")
    mode_group.add_argument("--managed", action="store_true", help="seed exact host-installed archives and verify managed paths in the guest")
    args = parser.parse_args()
    boot, build = args.boot.resolve(), args.build.resolve()
    fingerprint = hashlib.sha256()
    fingerprint.update(sha(boot / "esp/EFI/BOOT/BOOTX64.EFI").encode())
    for app in sorted((build / "apps").glob("*.fxe")):
        fingerprint.update(app.name.encode()); fingerprint.update(sha(app).encode())
    mode = "packages" if args.packages else "managed" if args.managed else "direct"
    if args.packages or args.managed:
        fingerprint.update(sha(build / "catalog/catalog.json").encode())
    output = build / ("qemu-" + boot.parent.name + "-" + mode + "-" + fingerprint.hexdigest()[:12])
    output.mkdir(parents=True, exist_ok=True)
    report = output / "result.json"
    if report.exists():
        raise RuntimeError("accepted evidence already exists; choose a fresh --build directory for a repeat")
    apps = build / "apps"
    available = {p.stem: sha(p) for p in apps.glob("*.fxe")}
    for name in ("mux", "rfl-fm", "keycheck"):
        if not (apps / (name + ".fxe")).is_file():
            raise RuntimeError("missing application " + name)
    capture = load_capture(ROOT / "build/x86_64-hp-persistent/gpt-after.json")
    identity = json.loads((ROOT / "build/x86_64-hp-persistent/root-identity.json").read_text())
    first = identity["first_lba"] * identity["sector_bytes"]
    image = boot / "usb-boot.img"
    serial = output / "serial.log"
    serial.unlink(missing_ok=True)
    successes = []
    extra_checks = []
    catalog = build / "catalog/catalog.json"
    if args.packages or args.managed:
        packages = json.loads(catalog.read_text())["packages"]
        if args.packages:
            for i, package in enumerate(packages):
                extra_checks.append((f"pkg-stage --adopt-seed install /tmp/tui{i}.rpk && echo PKG_{i}_OK",
                                     rf"\r?\nPKG_{i}_OK\r?\n"))
        extra_checks += [("pkg-stage list", r"rfl-fm 0\.")]
        successes.append("guest package installation and managed /usr paths" if args.packages else
                         "guest package DB and managed /usr paths from byte-identical host-installed archives")

    def terminal_checks(shell, type_text, qmp, wait_for, transcript):
        def press(code, control=False):
            codes = ([('ctrl', True)] if control else []) + [(code, True), (code, False)] + ([('ctrl', False)] if control else [])
            qmp("input-send-event", {"events": [
                {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": key}}}
                for key, down in codes]})
            time.sleep(.25)

        start = len(transcript())
        type_text("keycheck --capture 17\n")
        wait_for(lambda text: "KEYCHECK_READY" in text[start:])
        for code in ("esc", "up", "down", "left", "right", *["f" + str(i) for i in range(1, 13)]):
            press(code)
        text = wait_for(lambda text: "keycheck: arrows=15/15 function-keys=4095/4095" in text[start:] and "fuse-x# " in text[start:])[start:]
        for label in ("Escape", "Up", "Down", "Left", "Right", *["F" + str(i) for i in range(1, 13)]):
            if not re.search(r"KEY: " + label + r"\r?\n", text):
                raise RuntimeError("missing decoded key " + label)
        successes.append("Escape, four arrows and F1-F12 through QMP/i8042/TTY/shared curses")
        print("PASS: keycheck all 17 keys, arrows=15/15 function-keys=4095/4095", flush=True)

        start = len(transcript())
        type_text(("mc" if args.packages or args.managed else "rfl-fm") + "\n")
        wait_for(lambda text: "10 Quit" in text[start:])
        qmp("screendump", {"filename": str(output / "commander.png"), "format": "png"})
        press("down"); press("up"); press("f10")
        wait_for(lambda text: "fuse-x# " in text[start:])
        shell("echo COMMANDER_RETURNED", r"\r?\nCOMMANDER_RETURNED\r?\n")
        successes.append("Commander start, navigation, F10 quit and shell restoration")

        if "rift" in available:
            start = len(transcript())
            type_text("rift\n")
            wait_for(lambda text: "F10 Menu" in text[start:])
            qmp("screendump", {"filename": str(output / "rift.png"), "format": "png"})
            type_text("iTUI_NATIVE_OK")
            press("esc")
            type_text(":")
            wait_for(lambda text: ": command" in text[start:])
            type_text("wq /tmp/tui-rift\n")
            wait_for(lambda text: "fuse-x# " in text[start:])
            shell("cat /tmp/tui-rift; echo", r"\r?\nTUI_NATIVE_OK\r?\n")
            shell("echo RIFT_RETURNED", r"\r?\nRIFT_RETURNED\r?\n")
            successes.append("Rift start, insertion, Escape, :wq save, file content and shell restoration")

        if "levee-fixed" in available:
            shell("echo abc > /tmp/tui-levee", "")
            start = len(transcript())
            type_text("levee-fixed /tmp/tui-levee\n")
            wait_for(lambda text: "abc" in text[start:])
            press("right")
            type_text("x")
            type_text(":wq\n")
            wait_for(lambda text: "fuse-x# " in text[start:])
            shell("cat /tmp/tui-levee", r"\r?\nac\r?\n")
            successes.append("Levee right-arrow motion, delete, :wq save and exact file content")

        if "muxcheck" in available:
            shell("muxcheck", r"MUXCHECK_OK 16 PTYs")
            successes.append("16 PTYs and descriptor capacity")
        start = len(transcript())
        type_text("mux\n")
        wait_for(lambda text: "1:shell 1" in text[start:])
        type_text("echo MUX_FIRST_$?\n")
        wait_for(lambda text: "MUX_FIRST_0" in text[start:])
        press("b", control=True); press("c")
        wait_for(lambda text: "2:shell 2" in text[start:])
        type_text("echo MUX_SECOND_$?\n")
        wait_for(lambda text: "MUX_SECOND_0" in text[start:])
        qmp("screendump", {"filename": str(output / "mux.png"), "format": "png"})
        press("b", control=True); press("d")
        wait_for(lambda text: "[mux: left session main]" in text[start:] and "fuse-x# " in text[start:])
        shell("mux list", r"main")
        shell("mux kill main", "")
        shell("echo MUX_RETURNED", r"\r?\nMUX_RETURNED\r?\n")
        successes.append("mux two tabs, child shells, detach/list/kill and shell restoration")

    with tempfile.TemporaryDirectory(prefix="tui-fixture-", dir=output) as temporary:
        work = Path(temporary)
        seed = work / "root.img"
        prepared = catalog
        if args.managed:
            prepared = work / "managed-root"
            prepared.mkdir()
            engine = ROOT / "build/pkg/host/pkg-engine"
            with (output / "archive-stage.log").open("w") as log:
                for package in packages:
                    subprocess.run([engine, "--root", prepared, "--target", "riplix-x64-c16", "install", package["archive"]],
                                   stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run(["node", ROOT / "tools/x86_64/tui-wave/prepare.mjs", boot / "root.img", seed, apps,
                        *([prepared, mode] if args.packages or args.managed else [])], cwd=ROOT, check=True)
        disk = work / "nvme.img"
        write_image(disk, capture)
        with seed.open("rb") as source, disk.open("r+b") as target:
            at = first
            while chunk := source.read(1024 ** 2):
                if any(chunk):
                    target.seek(at); target.write(chunk)
                at += len(chunk)
        code, variables = firmware(SimpleNamespace(firmware_code=None, firmware_vars=None))
        shutil.copyfile(variables, work / "vars.fd")
        command = [executable("qemu-system-x86_64"), "-machine", "q35", "-accel", "tcg", "-cpu", "max", "-m", "1024M", "-smp", "1", "-no-reboot", "-nic", "none", "-monitor", "none", "-display", "none",
                   "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
                   "-drive", f"if=pflash,format=raw,file={work / 'vars.fd'}",
                   "-serial", f"file:{serial}", "-device", "qemu-xhci",
                   "-drive", f"if=none,id=bootusb,format=raw,snapshot=on,file={image}",
                   "-device", "usb-storage,drive=bootusb,removable=on",
                   "-blockdev", json.dumps({"driver": "raw", "node-name": "tui-root", "file": {"driver": "file", "filename": str(disk)}}),
                   "-device", "nvme,drive=tui-root,serial=RIPLIXFIXTURE,logical_block_size=512,physical_block_size=512"]
        with (output / "qemu.log").open("w") as log:
            result = run_shell_check(command, serial, log, args.timeout, output / "final.png", persistent=True,
                                     storage="nvme", debug_exit=False, checks_only=True, existing_root=True,
                                     system_services=True, extra_checks=extra_checks, terminal_checks=terminal_checks)
        if result.returncode:
            raise RuntimeError("QEMU exit status " + str(result.returncode))
    if available != {p.stem: sha(p) for p in apps.glob("*.fxe")}:
        raise RuntimeError("application inputs changed during acceptance")
    result = dict(status="passed", efi_sha256=sha(boot / "esp/EFI/BOOT/BOOTX64.EFI"),
                  applications={p.stem: dict(sha256=sha(p), bytes=p.stat().st_size) for p in apps.glob("*.fxe")},
                  checks=successes, physical_acceptance=False,
                  limitations="No AX211, physical Fn/media keys, audio, pointing device or SSH acceptance")
    report.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2), flush=True)


if __name__ == "__main__":
    main()
