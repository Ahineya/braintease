#!/usr/bin/env python3
"""Run RFL and its Commander against real kernels using private, disposable disks."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time
import tty

ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "ports/fuzix"


def run(args):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=300)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout


class Screen:
    """Small ANSI text sink: assert the rendered screen, not redraw byte patterns."""
    def __init__(self):
        self.rows, self.columns = 24, 80
        self.lines = [list(" " * self.columns) for _ in range(self.rows)]
        self.y = self.x = 0
        self.pending = ""
        self.wrap = False
        self.charset = False
        self.alternate, self.saved = False, None
        self.style = (7, 0, False, False, False)
        self.styles = [[self.style for _ in range(self.columns)] for _ in range(self.rows)]

    def feed(self, data):
        self.pending += data.decode("latin1")
        text, i = self.pending, 0
        while i < len(text):
            ch = text[i]
            if ch == "\x1b":
                if i + 1 == len(text): break
                if text[i + 1] == "[":
                    end = i + 2
                    while end < len(text) and not ("@" <= text[end] <= "~"): end += 1
                    if end == len(text): break
                    parameters, final = text[i + 2:end], text[end]
                    if parameters == "?1049" and final == "h" and not self.alternate:
                        self.saved = (self.lines, self.styles, self.y, self.x, self.style)
                        self.lines = [list(" " * self.columns) for _ in range(self.rows)]
                        self.styles = [[self.style for _ in range(self.columns)] for _ in range(self.rows)]
                        self.y = self.x = 0
                        self.wrap, self.alternate = False, True
                    elif parameters == "?1049" and final == "l" and self.alternate:
                        self.lines, self.styles, self.y, self.x, self.style = self.saved
                        self.saved, self.alternate, self.wrap = None, False, False
                    elif not parameters.startswith("?"):
                        numbers = [int(n) if n else 0 for n in parameters.split(";")]
                        n = numbers[0]
                        if final == "m":
                            fg, bg, bold, reverse, underline = self.style
                            at = 0
                            while at < len(numbers):
                                a = numbers[at]
                                if a == 0: fg, bg, bold, reverse, underline = 7, 0, False, False, False
                                elif a == 1: bold = True
                                elif a == 4: underline = True
                                elif a == 7: reverse = True
                                elif a == 22: bold = False
                                elif a == 24: underline = False
                                elif a == 27: reverse = False
                                elif 30 <= a <= 37: fg = a - 30
                                elif 40 <= a <= 47: bg = a - 40
                                elif 90 <= a <= 97: fg = a - 90 + 8
                                elif 100 <= a <= 107: bg = a - 100 + 8
                                elif a == 39: fg = 7
                                elif a == 49: bg = 0
                                elif a in (38, 48) and at + 2 < len(numbers) and numbers[at + 1] == 5:
                                    if a == 38: fg = numbers[at + 2]
                                    else: bg = numbers[at + 2]
                                    at += 2
                                at += 1
                            self.style = fg, bg, bold, reverse, underline
                        elif final in "Hf": self.y, self.x = max(0, n - 1), max(0, (numbers[1] if len(numbers) > 1 else 1) - 1)
                        elif final == "A": self.y = max(0, self.y - (n or 1))
                        elif final == "B": self.y = min(self.rows - 1, self.y + (n or 1))
                        elif final == "C": self.x = min(self.columns - 1, self.x + (n or 1))
                        elif final == "D": self.x = max(0, self.x - (n or 1))
                        elif final == "J" and n == 2: self.lines = [list(" " * self.columns) for _ in range(self.rows)]
                        elif final == "K" and self.y < self.rows:
                            for column in range(self.x if n == 0 else 0, self.columns if n != 1 else self.x + 1): self.lines[self.y][column] = " "
                        if final in "HfABCDJK": self.wrap = False
                    i = end + 1
                    continue
                if text[i + 1] in "()":
                    if i + 2 == len(text): break
                    if text[i + 1] == "(": self.charset = text[i + 2] == "0"
                    i += 3
                else: i += 2
                continue
            if ch == "\r": self.x, self.wrap = 0, False
            elif ch == "\n": self.y += 1; self.wrap = False
            elif ch == "\b": self.x = max(0, self.x - 1)
            elif ch >= " ":
                if self.wrap: self.x = 0; self.y += 1; self.wrap = False
                if self.y >= self.rows: self.lines.pop(0); self.lines.append(list(" " * self.columns)); self.styles.pop(0); self.styles.append([self.style] * self.columns); self.y = self.rows - 1
                if self.x < self.columns:
                    self.lines[self.y][self.x] = {"l": "┌", "k": "┐", "m": "└", "j": "┘", "q": "─", "x": "│", "t": "├", "u": "┤", "v": "┴", "w": "┬", "n": "┼"}.get(ch, ch) if self.charset else ch
                    self.styles[self.y][self.x] = self.style
                if self.x == self.columns - 1: self.wrap = True
                else: self.x += 1
            if self.y >= self.rows: self.lines.pop(0); self.lines.append(list(" " * self.columns)); self.styles.pop(0); self.styles.append([self.style] * self.columns); self.y = self.rows - 1
            i += 1
        self.pending = text[i:]

    def text(self): return "\n".join("".join(line) for line in self.lines)

    def resize(self, rows, columns):
        def grid(values, default):
            return [(values[y][:columns] if y < len(values) else []) +
                    [default] * max(0, columns - (len(values[y][:columns]) if y < len(values) else 0))
                    for y in range(rows)]
        self.lines, self.styles = grid(self.lines, " "), grid(self.styles, self.style)
        if self.saved:
            lines, styles, y, x, style = self.saved
            self.saved = grid(lines, " "), grid(styles, style), min(y, rows - 1), min(x, columns - 1), style
        self.rows, self.columns = rows, columns
        self.y, self.x = min(self.y, rows - 1), min(self.x, columns - 1)
        self.wrap = False


class Guest:
    def __init__(self, command, system):
        self.master, slave = pty.openpty()
        # The guest owns canonical/echo handling; do not add a host line discipline.
        tty.setraw(slave)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        environment = dict(os.environ, RCC_NATIVE_HEADLESS="1", RCC_NATIVE_NET="off", RCC_NATIVE_TIMEOUT="240")
        self.process = subprocess.Popen(list(map(str, command)), stdin=slave, stdout=slave,
                                        stderr=slave, env=environment, start_new_session=True)
        os.close(slave)
        self.log, self.at, self.screen = bytearray(), 0, Screen()
        self.prompt = b"fuzix# " if system == "fuzix" else b"fuse-x# "
        self.system = system

    def pump(self):
        if select.select([self.master], [], [], .1)[0]:
            try: data = os.read(self.master, 65536)
            except OSError: data = b""
            self.log.extend(data)
            self.screen.feed(data)

    def wait(self, condition, description, seconds=60):
        deadline = time.monotonic() + seconds
        while not condition():
            self.pump()
            if self.process.poll() is not None or time.monotonic() > deadline:
                raise AssertionError(description + "\n" + self.screen.text() + "\n" + self.log[-4000:].decode(errors="replace"))

    def raw(self, marker, seconds=60):
        self.wait(lambda: marker in self.log[self.at:], repr(marker), seconds)
        end = self.log.index(marker, self.at) + len(marker)
        result = bytes(self.log[self.at:end]).replace(b"\r", b"")
        self.at = end
        return result

    def visible(self, text): self.wait(lambda: text in self.screen.text(), text)
    def send(self, data): os.write(self.master, data)

    def resize(self, rows, columns):
        self.screen.resize(rows, columns)
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        os.kill(self.process.pid, signal.SIGWINCH)

    def shell(self, command):
        self.send(command.encode() + b"\r")
        return self.raw(self.prompt)

    def close(self):
        Path(f"/tmp/rfl-os-{self.system}.log").write_bytes(self.log)
        if self.process.poll() is None: self.process.kill(); self.process.wait()
        os.close(self.master)


def exercise(args, system, directory):
    build = (args.fuzix_build if system == "fuzix" else args.native_build).resolve()
    extension = "rxe" if system == "fuzix" else "fxe"
    artifacts = {}
    for name, source in [("rfl-probe", "tests/fixtures/os.rfl"), ("rfl-fm", "apps/commander/main.rfl"),
                         ("rfl-panic", "tests/fixtures/curses_failure.rfl")]:
        output = (args.prebuilt.resolve() / system if args.prebuilt else directory) / (name + "." + extension)
        if args.prebuilt:
            assert output.is_file(), output
        else:
            run([args.rfl.resolve(), "build", ROOT / "rfl" / source, "--os", system,
                 "--build-dir", build, "-o", output])
        if args.save_artifacts:
            import shutil
            saved = args.save_artifacts.resolve() / system
            saved.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(output, saved / output.name)
        artifacts[name] = output
    image = directory / "disk.img"
    if system == "fuzix":
        stage = directory / "stage"
        stage.mkdir()
        names = ["init", "sh", "echo", "cat", "env", "true", "stty", "levee", "vi", "chmod", "sleep"]
        for name in [*(n + ".rxe" for n in names), "FUZIX.SYS", "bios.bin"]: (stage / name).symlink_to(build / name)
        for name, path in artifacts.items(): (stage / (name + ".rxe")).symlink_to(path)
        (stage / "apps.json").write_text(json.dumps(names + list(artifacts) + ["mc"]))
        run(["node", PORT / "tools/disk.mjs", stage, image])
        command = [ROOT / "target/release/rvm", build / "bios.bin", "--disk", image,
                   "--memory", "16384000", "--net", "off", "--jit", "--guest-ctrl-c", "--timeout", "240"]
    else:
        # Seed Commander and its alias together, including this test's build.
        apps = directory / "native-apps"
        apps.mkdir()
        for path in (build / "apps").glob("*.fxe"):
            (apps / path.name).symlink_to(path)
        commander = apps / "rfl-fm.fxe"
        commander.unlink(missing_ok=True)
        commander.symlink_to(artifacts["rfl-fm"])
        report = build / "application-report.json"
        if report.exists():
            (directory / report.name).symlink_to(report)
        run(["node", ROOT / "fuse-x/image.mjs", "create", image, "--apps", apps])
        for name, path in artifacts.items():
            if name != "rfl-fm":
                run(["node", ROOT / "fuse-x/image.mjs", "put", image, "/bin/" + name, path])
        share = directory / "Host folder"
        share.mkdir()
        (share / "input.bin").write_bytes(bytes(i % 251 for i in range(70000)))
        (share / "input.bin").chmod(0o700)
        (share / "keep.bin").write_bytes(b"KEEP")
        locked = share / "locked"
        locked.mkdir()
        (locked / "file").write_bytes(b"KEEP")
        locked.chmod(0o500)
        command = [build / "fuse-x", "--run", image, "--share", share]
    guest = Guest(command, system)
    try:
        guest.raw(guest.prompt)
        output = guest.shell("rfl-probe; echo RFL_STATUS:$?")
        assert b"RFL_OS_OK" in output and b"RFL_STATUS:0" in output, output
        assert b"STATUS:23" in guest.shell("rfl-probe status; echo STATUS:$?")
        assert b"STATUS:1" in guest.shell("rfl-probe panic; echo STATUS:$?")
        assert b"RFL_PIPE_OK" in guest.shell("echo RFL_PIPE_OK | rfl-probe copy")
        output = guest.shell("rfl-panic; echo CURSES_STATUS:$?")
        assert b"CURSES_STATUS:1" in output and b"RFL_CURSES_FAILURE_TEST" in output, output
        assert b"RFL_WINDOW_API_OK" in output and b"RFL_WINDOW_FAILED" not in output, output
        print(system + ": process exit/panic isolation, stdio, pipes, args/env, RAII, 70K binary files, seek/dup and copy safeguards passed", flush=True)
        if system == "fuse-x":
            output = guest.shell("rfl-probe host-moves; echo MOVE_STATUS:$?")
            assert b"RFL_HOST_MOVES_OK" in output and b"MOVE_STATUS:0" in output, output
            assert not (share / "input.bin").exists()
            assert (share / "returned.bin").read_bytes() == bytes(i % 251 for i in range(70000))
            assert (share / "returned.bin").stat().st_mode & 0o777 == 0o700
            assert (share / "keep.bin").read_bytes() == b"KEEP"
            assert (locked / "file").read_bytes() == b"KEEP"
        assert b"/bin/vi" in guest.shell("echo $EDITOR")
        guest.send(b"mc /left /right\r")
        guest.visible("RFL Commander")
        guest.visible("Selected: /")
        guest.wait(lambda: guest.screen.styles[4][1][:2] == (0, 6), "classic cyan selection on dark blue")
        assert guest.screen.styles[5][1][1] == 4
        assert guest.screen.lines[1][0] == "┌", guest.screen.text()
        guest.send(b"\x01")
        guest.visible("About RFL Commander")
        guest.visible("A two-panel file manager written in RFL.")
        guest.send(b"\r")
        guest.wait(lambda: "About RFL Commander" not in guest.screen.text(), "About dismissed")
        guest.send(b"\x1b[20~")  # F9 theme chooser.
        guest.visible("Choose theme")
        guest.send(b"g\r")
        guest.visible("Theme: Graphite")
        assert guest.screen.styles[4][1][:2] == (0, 7), guest.screen.styles[4][1]
        guest.send(b"\x1b[20~")
        guest.visible("Choose theme")
        guest.send(b"\r")  # Reload system/user configuration.
        guest.visible("Theme: Classic Blue")
        guest.wait(lambda: guest.screen.styles[4][1][:2] == (0, 6), "configured theme restored")
        guest.send(b"\x1b[B")
        guest.visible("Selected: /left/adir")
        assert guest.screen.styles[5][1][:2] == (0, 6) and guest.screen.styles[4][1][1] == 4
        guest.send(b"\r")
        guest.visible("[ /left/adir ]")
        guest.send(b"\x1b[D")
        guest.visible("[ /left ]")
        guest.send(b"\x1b[B\x1b[B")
        guest.visible("Selected: /left/alpha.txt")
        guest.send(b"\x1bOR")  # F3 viewer.
        guest.visible("View: /left/alpha.txt")
        guest.visible("RFL file manager fixture")
        guest.send(b"q")
        guest.wait(lambda: "View: /left/alpha.txt" not in guest.screen.text(), "floating viewer dismissed")
        guest.send(b"\x1bOS")  # F4: real system editor (vi -> Levee).
        guest.wait(lambda: not guest.screen.alternate, "editor receives the normal terminal")
        guest.visible("second line")
        guest.send(b"iEDITED \x1b:wq\r")
        guest.visible("Returned from editor")
        guest.send(b"\x1b[15~")  # F5 copy.
        guest.visible("Copy to: /right/alpha.txt")
        guest.send(b"\r")
        guest.visible("Copied 44 bytes")
        guest.send(b"\t")
        guest.visible("Selected: /")
        guest.send(b"\x1b[B")
        guest.visible("Selected: /right/alpha.txt")
        guest.send(b"\x1b[17~")  # F6 rename/move.
        guest.visible("Move to:")
        guest.send(b"\x15/right/renamed.txt\r")
        guest.visible("Moved successfully")
        guest.send(b"r")
        guest.visible("Rename: renamed.txt")
        guest.send(b"\x15local.txt\r")
        guest.visible("Renamed successfully")
        guest.visible("Selected: /right/local.txt")
        guest.send(b"r")
        guest.visible("Rename: local.txt")
        guest.send(b"\x15renamed.txt\r")
        guest.visible("Renamed successfully")
        guest.send(b"\x1b[18~")  # F7 mkdir.
        guest.visible("New directory:")
        guest.send(b"newdir\r")
        guest.visible("Directory created")
        guest.send(b"\x1b[F")
        guest.visible("Selected: /right/renamed.txt")
        guest.send(b"\x1b[19~")  # F8 delete, cancel once.
        guest.visible("Delete renamed.txt?")
        guest.send(b"n")
        guest.visible("Selected: /right/renamed.txt")
        guest.send(b"\x1b[19~")
        guest.visible("Delete renamed.txt?")
        guest.send(b"y")
        guest.visible("Deleted successfully")
        guest.send(b"\x10")  # Ctrl-P single panel.
        guest.wait(lambda: "[ /right ]" in guest.screen.text() and " /left " not in guest.screen.text().splitlines()[1], "single panel")
        guest.send(b"\x10\x0f")  # Return to two panels, Ctrl-O shell.
        guest.visible("RFL Commander shell")
        guest.visible("rfl$ ")
        guest.send(b"echo RFL_SHELL_OK\r")
        guest.wait(lambda: any(line.strip() == "RFL_SHELL_OK" for line in guest.screen.text().splitlines()), "guest shell command output")
        guest.send(b"\r")
        guest.visible("Returned from guest shell")
        guest.send(b"\x1b[21~")  # F10 quit.
        guest.raw(guest.prompt)
        assert not guest.screen.alternate and "[ /right ]" not in guest.screen.text()
        assert b"EDITED RFL file manager fixture" in guest.shell("cat /left/alpha.txt")
        guest.send(b"mc /launch /right\r")
        guest.visible("[ /launch ]")
        guest.send(b"\x1b[B\r")
        guest.visible("RFL_EXEC_OK")
        guest.visible("Press Enter to return")
        assert not guest.screen.alternate
        guest.send(b"\r")
        guest.visible("Program finished")
        guest.send(b"\x1b[21~")
        guest.raw(guest.prompt)
        guest.shell("echo 'echo RFL_TEXT_OK' > /launch/text-script; chmod 755 /launch/text-script")
        guest.send(b"mc /launch /right\r")
        guest.visible("[ /launch ]")
        guest.at = len(guest.log)
        guest.send(b"\x1b[F\r")
        guest.raw(b"RFL_TEXT_OK")
        guest.raw(b"Press Enter to return")
        guest.send(b"\r")
        guest.visible("Program finished")
        guest.send(b"\x1b[21~")
        guest.raw(guest.prompt)
        guest.shell("echo 'echo RFL_INTERRUPT_READY; sleep 10' > /launch/z-interrupt; chmod 755 /launch/z-interrupt")
        guest.send(b"mc /launch /right\r")
        guest.visible("[ /launch ]")
        guest.at = len(guest.log)
        guest.send(b"\x1b[F\r")
        guest.raw(b"RFL_INTERRUPT_READY")
        guest.send(b"\x03")
        guest.raw(b"Press Enter to return", seconds=5)
        guest.send(b"\r")
        guest.visible("Program wait status:")
        guest.send(b"\x1b[21~")
        guest.raw(guest.prompt)
        guest.shell("VISUAL='sh /tmp/test-editor'; export VISUAL")
        guest.send(b"mc /edit /right\r")
        guest.visible("[ /edit ]")
        guest.send(b"\x1b[B\x1bOS")
        guest.visible("Program wait status:")
        assert "View:" not in guest.screen.text()
        guest.send(b"\x1b[21~")
        guest.raw(guest.prompt)
        # Inspect through RFL rather than letting the test shell expand $xx.
        assert b"EDITED" in guest.shell("cat '/edit/a'\"'\"'b $xx.txt'")
        guest.shell("unset VISUAL")
        assert b"RESTORED" in guest.shell("echo RESTORED")
        guest.send(b"mc /scroll /right\r")
        guest.visible("[ /scroll ]")
        guest.send(b"\x1b[20~")
        guest.visible("Choose theme")
        guest.resize(6, 30)
        guest.visible("terminal needs")
        guest.resize(24, 80)
        guest.visible("[ /scroll ]")
        guest.send(b"\x1b[6~")
        guest.wait(lambda: guest.screen.lines[19][1] == ">" and any("#" in line[38:39] for line in guest.screen.lines[4:20]), "paged selection and scrollbar")
        guest.send(b"\x1b[F")
        guest.visible("Selected: /scroll/item-9")
        guest.send(b"\x1b[H")
        guest.visible("Selected: /")
        guest.send(b"\x1b[20~")
        guest.visible("Choose theme")
        guest.send(b"m\r")
        guest.visible("Theme: Monochrome")
        assert guest.screen.lines[1][0] == "+" and guest.screen.styles[4][1][3]
        guest.send(b"\x1b[21~")
        guest.raw(guest.prompt)
        guest.send(b"shutdown\r")
        guest.process.wait(timeout=30)
        assert guest.process.returncode == 0
        print(system + ": live two-panel navigation, About, viewer, real editor, quoted filenames, rename, binary/script launch, child interrupt, F5/F6/F7/F8/F10, cancellation, one-panel layout, guest shell and terminal restoration passed", flush=True)
    finally:
        guest.close()
        if system == "fuse-x": locked.chmod(0o700)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--os", choices=("fuzix", "fuse-x", "both"), default="both")
    parser.add_argument("--rfl", type=Path, default=ROOT / "target/release/rfl")
    parser.add_argument("--fuzix-build", type=Path, default=ROOT / "build/fuzix")
    parser.add_argument("--native-build", type=Path, default=ROOT / "fuse-x/build")
    parser.add_argument("--prebuilt", type=Path, help="explicitly reuse previously saved test executables")
    parser.add_argument("--save-artifacts", type=Path, help="save this run's fresh executables for focused reruns")
    args = parser.parse_args()
    for system in (["fuse-x", "fuzix"] if args.os == "both" else [args.os]):
        with tempfile.TemporaryDirectory(prefix="rfl-os-", dir="/tmp") as temporary:
            exercise(args, system, Path(temporary))


if __name__ == "__main__": main()
