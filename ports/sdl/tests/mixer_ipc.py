#!/usr/bin/env python3
"""Independent helper-process mixer acceptance using real Fuse-X pipes and PCM.

Build ports/sdl/build.py --app mixer-ipc and --app mixer-helper first. Every
case boots a disposable private root; physical speakers and networking stay
muted. The scalar oracle checks each rendered sample, so a gap, skipped block,
repeated block or incorrect channel ordering fails even when Mix_Playing is 1.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "ports/tls/tests"))
from guest import Guest

RATE = 8000
FRAMES = 4096
CASES = ("block", "busy", "partial", "flood", "controls", "late-poll", "failed-launch", "kill-helper", "abrupt")
CAPTURE = {"block", "busy", "partial", "flood", "abrupt"}


def source_frame(frame):
    frame %= FRAMES
    return ((frame * 37) % 251) * 83 - 10000, ((frame * 53 + 19) % 241) * 97 - 11000


def check_capture(path, case):
    with wave.open(str(path), "rb") as recording:
        assert (recording.getframerate(), recording.getnchannels(), recording.getsampwidth()) == (RATE, 2, 2)
        count = recording.getnframes()
        raw = recording.readframes(count)
    samples = struct.unpack("<" + "h" * (len(raw) // 2), raw)
    frames = list(zip(samples[::2], samples[1::2]))
    active = [at for at, frame in enumerate(frames) if frame != (0, 0)]
    assert active, (case, "capture contains no rendered waveform")
    first, last = active[0], active[-1]
    audible = frames[first:last + 1]
    # First/last silence is the deliberately idle mixer. Silence inside the
    # active interval, including PCM underrun fill, must never be discarded.
    for at, actual in enumerate(audible):
        expected = source_frame(at)
        assert actual == expected, dict(case=case, source_frame=at, capture_frame=first + at,
                                        actual=actual, expected=expected,
                                        unexpected_silence=actual == (0, 0))
    minimum = 5000 if case == "abrupt" else 7500
    assert len(audible) >= minimum, (case, len(audible), "too little PCM to cover isolated work")
    assert all(frame == (0, 0) for frame in frames[:first])
    assert all(frame == (0, 0) for frame in frames[last + 1:])
    return dict(frames=count, uninterrupted_frames=len(audible),
                uninterrupted_ms=len(audible) * 1000 / RATE,
                leading_silence_frames=first, trailing_silence_frames=len(frames) - last - 1,
                sample_sha256=hashlib.sha256(raw).hexdigest())


def command_for(case):
    if case in ("block", "busy"):
        return "--capture " + case
    if case in ("partial", "flood"):
        return "--raw " + case
    return "--" + case


def run_case(args, case):
    with tempfile.TemporaryDirectory(prefix="sdl-mixer-ipc-" + case + "-") as directory:
        work = Path(directory)
        capture = work / "output.wav"
        saved = dict(os.environ)
        guest = None
        try:
            os.environ.update(RCC_NATIVE_AUDIO="off", RCC_NATIVE_NET="off")
            if case in CAPTURE:
                os.environ["RCC_NATIVE_AUDIO_WAV"] = str(capture)
            else:
                os.environ.pop("RCC_NATIVE_AUDIO_WAV", None)
            guest = Guest(work, "native", {"sdl-mixer-ipc": args.guest, "sdl-mixerd": args.helper},
                          [dict(name="/dev/pcm", mode=0o20666, device=0x500)], blocks=65536)
        except BaseException:
            if (work / "guest.log").exists():
                shutil.copyfile(work / "guest.log", args.output / (case + ".log"))
            raise
        finally:
            os.environ.clear()
            os.environ.update(saved)
        result = dict(case=case)
        try:
            output = guest.shell("sdl-mixer-ipc " + command_for(case) + "; echo SDL_MIXER_IPC_STATUS:$?", seconds=180)
            assert "SDL_MIXER_IPC_STATUS:0" in output, output
            assert "SDL_MIXER_IPC_FAIL line " not in output, output
            result["fixture"] = [line for line in output.splitlines() if line.startswith("SDL_MIXER_IPC_")]
            if case in ("block", "busy"):
                elapsed = re.search(r"elapsed=(\d+)", output)
                assert elapsed and int(elapsed[1]) >= 650, output
                result["isolated_parent_ms"] = int(elapsed[1])
            if case == "late-poll":
                phases = re.findall(r"SDL_MIXER_IPC_LATE_POLL phase=(\d+) elapsed=(\d+) helper=(\d+)", output)
                assert len(phases) == 2 and [int(item[0]) for item in phases] == [0, 1], output
                assert all(int(item[1]) >= 10900 for item in phases) and phases[0][2] == phases[1][2], output
                result.update(late_poll_parent_ms=[int(item[1]) for item in phases], helper_pid=int(phases[0][2]))
            if case == "flood":
                written = re.search(r"bytes=(\d+) retries=(\d+)", output)
                assert written and int(written[1]) > 8192 and int(written[2]) > 0, output
                result.update(unread_reply_flood_octets=int(written[1]), backpressure_retries=int(written[2]))
            # A fresh process must be able to reacquire the exclusive PCM lease
            # after each path, especially abrupt parent death and reply loss.
            recovery = guest.shell("sdl-mixer-ipc --probe; echo SDL_MIXER_IPC_PROBE_STATUS:$?", seconds=180)
            assert "SDL_MIXER_IPC_PROBE_STATUS:0" in recovery and "SDL_MIXER_IPC_PROBE passed" in recovery, recovery
            result["lease_reacquired"] = True
            assert not any(marker in guest.log for marker in
                           (b"native runtime error", b"panic:", b"SDL_MIXER_IPC_FAIL line ", b"Audio stream error")), \
                guest.log[-8000:].decode(errors="replace")
        finally:
            try:
                guest.close()  # Flushes and checks the disposable root image.
            finally:
                (args.output / (case + ".log")).write_bytes(guest.log)
                if capture.exists():
                    shutil.copyfile(capture, args.output / (case + ".wav"))
        if case in CAPTURE:
            result["pcm"] = check_capture(args.output / (case + ".wav"), case)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--guest", type=Path, default=ROOT / "build/sdl-mixer-ipc/sdl-mixer-ipc.fxe")
    parser.add_argument("--helper", type=Path, default=ROOT / "build/sdl-mixer-helper/sdl-mixerd.fxe")
    parser.add_argument("--output", type=Path, default=ROOT / "build/sdl-mixer-ipc-tests")
    parser.add_argument("--case", choices=("all", *CASES), default="all")
    args = parser.parse_args()
    for name in ("guest", "helper", "output"):
        setattr(args, name, getattr(args, name).resolve())
    assert args.guest.is_file(), "Build --app mixer-ipc first: " + str(args.guest)
    assert args.helper.is_file(), "Build --app mixer-helper first: " + str(args.helper)
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(status="running", cases=[],
                  fixture_sha256=hashlib.sha256(args.guest.read_bytes()).hexdigest(),
                  helper_sha256=hashlib.sha256(args.helper.read_bytes()).hexdigest(),
                  fixture_source_sha256=hashlib.sha256((ROOT / "ports/sdl/tests/mixer-ipc.c").read_bytes()).hexdigest())
    try:
        for case in CASES if args.case == "all" else (args.case,):
            print("SDL mixer IPC: " + case, flush=True)
            report["cases"].append(run_case(args, case))
            (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        report["status"] = "passed"
    except BaseException as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        (args.output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print("SDL mixer IPC: exact uninterrupted real PCM and process/pipe/lease cleanup passed")


if __name__ == "__main__":
    main()
