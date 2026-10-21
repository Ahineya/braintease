#!/usr/bin/env python3
"""Exercise cooperative SDL callbacks and exact PCM on disposable Fuse-X disks.

Build separately with ports/sdl/build.py --app audio --build build/sdl-audio.
All speaker output is muted. Captures validate actual /dev/pcm conversion,
including signedness, stereo ordering, endian representation and pause/resume.
The optional host executable checks controls only; it is not a guest substitute.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import wave

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "ports/tls/tests"))
from guest import Guest

FORMATS = ("u8", "s8", "u16le", "u16be", "s16le", "s16be")
EDGES = (-32768, -30001, -16384, -257, -1, 1, 257, 12345, 30001, 32767)


def expected_sample(frame, channel, format_name):
    if frame < 10:
        sample = EDGES[(frame + channel * 7) % 10]
    else:
        sample = ((frame * 37 + 19 + channel * 83) % 251) * 257 - 32123 + channel * 37
    # The callback serializes the upper octet for 8-bit formats. Both signed
    # and unsigned encodings must recover this same signed S16 amplitude.
    return (sample // 256) * 256 if format_name in ("u8", "s8") else sample


def check_capture(path, format_name, generated, rate=8000, channels=2):
    with wave.open(str(path), "rb") as recording:
        assert (recording.getframerate(), recording.getnchannels(), recording.getsampwidth()) == (rate, channels, 2)
        count = recording.getnframes()
        raw = recording.readframes(count)
    assert 512 <= count <= generated, (format_name, count, generated)
    samples = struct.unpack("<" + "h" * (len(raw) // 2), raw)
    for index, actual in enumerate(samples):
        expected = expected_sample(index // channels, index % channels, format_name)
        assert actual == expected, dict(format=format_name, frame=index // channels,
                                        channel=index % channels, actual=actual, expected=expected)
    return dict(format=format_name, rate=rate, channels=channels, frames=count, generated=generated,
                sample_sha256=hashlib.sha256(raw).hexdigest())


def run_guest(args, name, mode, with_window=False, capture_format=None, rate=8000, channels=2):
    with tempfile.TemporaryDirectory(prefix="sdl-audio-" + name + "-") as directory:
        work = Path(directory)
        capture = work / "output.wav"
        frames, mailbox = work / "frames", work / "input.json"
        saved = dict(os.environ)
        os.environ.update(RCC_NATIVE_AUDIO="off", RCC_NATIVE_AUDIO_WAV=str(capture),
                          RCC_NATIVE_NET="off", FUSE_X_FRAMES=str(frames),
                          FUSE_X_TEST_EVENT=str(mailbox))
        files = [dict(name="/dev/pcm", mode=0o20666, device=0x500)]
        if with_window:
            files.append(dict(name="/dev/gfx", mode=0o20600, device=0x400))
        guest = None
        try:
            guest = Guest(work, "native", {"sdl-audio": args.guest}, files, blocks=65536)
        except BaseException:
            startup_log = work / "guest.log"
            if startup_log.exists():
                shutil.copyfile(startup_log, args.output / (name + ".log"))
            raise
        finally:
            os.environ.clear()
            os.environ.update(saved)

        def wait(condition, seconds=90):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                guest.pump()
                result = condition()
                if result:
                    return result
                assert guest.process.poll() is None, guest.log[-8000:].decode(errors="replace")
            raise AssertionError("SDL audio wait timed out:\n" + guest.log[-8000:].decode(errors="replace"))

        def windows():
            try:
                return json.loads((frames / "windows.json").read_text())
            except (FileNotFoundError, json.JSONDecodeError):
                return []

        result = dict(case=name)
        try:
            if with_window:
                guest.shell("sdl-audio --wait > /audio-wait.log &")
                guest.shell("audio_pid=$!")
                # The fixture's stdout is redirected so the shell prompt and
                # READY marker cannot race each other in the host log reader.
                def ready():
                    output = guest.shell("cat /audio-wait.log")
                    return output if "SDL_AUDIO_WAIT_READY" in output else None
                wait(ready)
                window = wait(lambda: next((item for item in windows()
                    if item["visible"] and item["title"] == "SDL audio wait test"), None))
                until = time.monotonic() + 0.7
                while time.monotonic() < until:
                    guest.pump()
                pending = mailbox.with_suffix(".tmp")
                pending.write_text(json.dumps(dict(window=window["id"],
                    input=dict(type="key", key=44, down=True))))
                pending.replace(mailbox)
                wait(lambda: not mailbox.exists())
                output = guest.shell("wait $audio_pid; echo SDL_AUDIO_STATUS:$?; cat /audio-wait.log")
                assert "SDL_AUDIO_STATUS:0" in output and "SDL_AUDIO_WAIT passed" in output, output
                match = re.search(r"SDL_AUDIO_WAIT passed before=(\d+) after=(\d+) elapsed=(\d+) generated=(\d+)", output)
                assert match, output
                before, after, elapsed, generated = map(int, match.groups())
                assert after >= before + 4 and elapsed >= 400, match.groups()
                result.update(before=before, after=after, elapsed_ms=elapsed)
                wait(lambda: not any(item["id"] == window["id"] for item in windows()))
            else:
                output = guest.shell("sdl-audio " + mode + "; echo SDL_AUDIO_STATUS:$?", seconds=180)
                assert "SDL_AUDIO_STATUS:0" in output, output
                if mode.startswith("--controls"):
                    assert "SDL_AUDIO_CONTROLS passed rate=" + str(rate) in output, output
                    result["rate"] = rate
                if capture_format:
                    match = re.search(r"SDL_AUDIO_CAPTURE format=" + capture_format + r" calls=(\d+) generated=(\d+)", output)
                    assert match and int(match[1]) >= 4, output
                    generated = int(match[2])
            assert not any(marker in guest.log for marker in
                (b"native runtime error", b"panic:", b"SDL_AUDIO_FAIL", b"Audio stream error")), \
                guest.log[-8000:].decode(errors="replace")
        finally:
            try:
                guest.close()  # Also checks the private root filesystem.
            finally:
                (args.output / (name + ".log")).write_bytes(guest.log)
        if capture_format or with_window:
            retained = args.output / (name + ".wav")
            shutil.copyfile(capture, retained)
            result.update(check_capture(retained, capture_format or "s16le", generated, rate, channels))
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--guest", type=Path, default=ROOT / "build/sdl-audio/sdl-audio.fxe")
    parser.add_argument("--host", type=Path, help="optional host executable for the controls case")
    parser.add_argument("--host-only", action="store_true")
    parser.add_argument("--output", type=Path, default=ROOT / "build/sdl-audio/tests")
    parser.add_argument("--case", choices=("all", "controls", "rates", "capture", "mono", "wait"), default="all")
    parser.add_argument("--formats", nargs="+", choices=FORMATS, default=list(FORMATS))
    args = parser.parse_args()
    args.guest = args.guest.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    report_path = args.output / "result.json"
    report_path.unlink(missing_ok=True)
    report = dict(status="running", checks=[])
    try:
        if args.host:
            host = subprocess.run([str(args.host.resolve()), "--controls"],
                env=dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy"),
                capture_output=True, text=True, timeout=30)
            (args.output / "host-controls.log").write_text(host.stdout + host.stderr)
            assert host.returncode == 0 and "SDL_AUDIO_CONTROLS passed" in host.stdout, host.stdout + host.stderr
            report["host_controls"] = "passed"
        if args.host_only:
            assert args.host, "--host-only requires --host"
        else:
            assert args.guest.is_file(), "Build the SDL audio fixture first: " + str(args.guest)
            report["guest_sha256"] = hashlib.sha256(args.guest.read_bytes()).hexdigest()
            if args.case in ("all", "controls"):
                report["checks"].append(run_guest(args, "controls", "--controls"))
            if args.case in ("all", "rates"):
                for rate in (44100, 48000):
                    report["checks"].append(run_guest(args, "controls-" + str(rate),
                                                       "--controls " + str(rate), rate=rate))
            if args.case in ("all", "capture"):
                for format_name in args.formats:
                    report["checks"].append(run_guest(args, format_name, "--capture " + format_name,
                                                       capture_format=format_name))
            if args.case in ("all", "mono"):
                for format_name in ("u8", "s16be"):
                    report["checks"].append(run_guest(args, format_name + "-mono",
                        "--capture " + format_name + " 1", capture_format=format_name, channels=1))
            if args.case in ("all", "wait"):
                report["checks"].append(run_guest(args, "wait", "--wait", with_window=True))
        report["status"] = "passed"
    except BaseException as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        report_path.write_text(json.dumps(report, indent=2) + "\n")
    print("SDL audio: controls, shared callback state, cooperative waits and exact PCM passed"
          if args.case == "all" and not args.host_only else "SDL audio: selected checks passed")


if __name__ == "__main__":
    main()
