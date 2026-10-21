#!/usr/bin/env python3
"""Check production SDL_mixer rendering and real guest PCM on private disks.

Build the ordinary fixture with ports/sdl/build.py --app mixer --build
build/sdl-mixer. This runner also builds a deterministic callback fixture with
the production mixer, WAV parser and converter on byte/wide host carriers and
on actual C16. Speaker output is muted; normal filesystem images are untouched.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
sys.path.insert(0, str(PORT))
from adapt import prepare

CASES = ("overlap", "loop", "volume", "pause")
EDGES = (24000, -24000, 28000, -28000, 12000, -12000, 30000, -30000)


def voice(which, frame, channel):
    base = EDGES[(frame + channel * 3) % 8]
    if which == 0:
        return base
    if which == 1:
        return 16000 if base > 0 else -16000
    return (-10000 if base > 0 else 10000) if frame % 3 == 0 else (4000 if base > 0 else -4000)


def expected_frame(case, frame):
    def sample(channel):
        value = voice(0, frame % 32, channel)
        if case == "overlap":
            value += voice(1, frame % 32, channel) + voice(2, frame % 32, channel)
        elif case == "volume":
            value //= 4  # All source values are exactly divisible by four.
        return max(-32768, min(32767, value))
    return sample(0), sample(1)


def check_capture(path, case):
    with wave.open(str(path), "rb") as recording:
        assert (recording.getframerate(), recording.getnchannels(), recording.getsampwidth()) == (8000, 2, 2)
        count = recording.getnframes()
        raw = recording.readframes(count)
    samples = struct.unpack("<" + "h" * (len(raw) // 2), raw)
    frames = list(zip(samples[::2], samples[1::2]))
    assert count >= 128, (case, count)
    active = [at for at, value in enumerate(frames) if value != (0, 0)]
    assert active, (case, "capture contains no rendered voice")
    first, last = active[0], active[-1]
    # OpenAudio can prefill silence before a voice is queued. After that, each
    # audible sample must match its exact source position, including clipping.
    source_frame = 0
    gaps = []
    silence_start = None
    for at in range(first, last + 1):
        actual = frames[at]
        if actual == (0, 0):
            assert case == "pause", dict(case=case, frame=at, unexpected_silence=True)
            if silence_start is None:
                silence_start = at
            continue
        if silence_start is not None:
            gaps.append(at - silence_start)
            silence_start = None
        expected = expected_frame(case, source_frame)
        assert actual == expected, dict(case=case, capture_frame=at, source_frame=source_frame,
                                        actual=actual, expected=expected)
        source_frame += 1
    assert all(value == (0, 0) for value in frames[:first])
    assert all(value == (0, 0) for value in frames[last + 1:])
    if case == "loop":
        assert source_frame == 96, (case, source_frame)
        assert len(frames) - last - 1 >= 64, (case, "finite loop did not terminate before capture ended")
    else:
        assert source_frame >= 512, (case, source_frame)
    if case == "pause":
        assert len(gaps) == 1 and gaps[0] >= 512, (case, gaps)
    else:
        assert not gaps, (case, gaps)
    return dict(case=case, frames=count, rendered_frames=source_frame, leading_silence=first,
                pause_gaps=gaps, sample_sha256=hashlib.sha256(raw).hexdigest())


def run(command, log, timeout=120, **kwargs):
    result = subprocess.run(command, capture_output=True, timeout=timeout, **kwargs)
    output = result.stdout + result.stderr
    if isinstance(output, bytes):
        log.write_bytes(output)
        readable = output.decode(errors="replace")
    else:
        log.write_text(output)
        readable = output
    if result.returncode:
        raise RuntimeError("Command failed; see " + str(log) + "\n" + readable[-7000:])
    return readable


def minimal_units(source):
    return [PORT / "mixer/mixer.c", PORT / "c16/audio_formats.c", source / "src/audio/SDL_audiocvt.c"]


def host_deterministic(args):
    records = {}
    for profile in ("octet8", "octet16"):
        work = args.output / profile
        work.mkdir(parents=True, exist_ok=True)
        source = prepare(work, False, False)
        if profile == "octet16":
            path = source / "include/SDL_stdinc.h"
            text = path.read_text()
            for before, after in (("typedef int8_t\t\tSint8;", "typedef int16_t\t\tSint8;"),
                                  ("typedef uint8_t\t\tUint8;", "typedef uint16_t\t\tUint8;"),
                                  ("sizeof(Uint8) == 1", "sizeof(Uint8) == 2"),
                                  ("sizeof(Sint8) == 1", "sizeof(Sint8) == 2")):
                assert before in text, before
                text = text.replace(before, after)
            path.write_text(text)
        includes = [source / "include", PORT / "mixer", PORT / "c16"]
        command = [*shlex.split(os.environ.get("CC", "cc")), "-std=c99", "-O2", "-fno-strict-aliasing", "-DMIXER_FAKE_AUDIO=1"]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        sdk = Path("/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk")
        if sys.platform == "darwin" and sdk.exists():
            command += ["-isysroot", str(sdk)]
        command += ["-I" + str(path) for path in includes]
        output = work / "mixer-fixture"
        command += [*map(str, minimal_units(source)), str(PORT / "tests/mixer.c"), "-o", str(output)]
        run(command, work / "build.log", text=True)
        record = run([str(output), "--deterministic"], work / "run.log", text=True, timeout=30)
        assert re.search(r"SDL_MIXER_DETERMINISTIC passed checks=\d+", record), record
        records[profile] = record
    assert records["octet8"] == records["octet16"], records
    return records["octet8"].strip()


def guest_deterministic_build(args):
    work = args.output / "c16"
    work.mkdir(parents=True, exist_ok=True)
    with (work / ".build.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        source = prepare(work, False, True)
        pinned = work / "rcc-pinned"
        if not pinned.exists() or pinned.read_bytes() != args.compiler.read_bytes():
            shutil.copy2(args.compiler, pinned)
        includes = [source / "include", source / "src", PORT / "mixer", PORT / "c16"]
        sdk = [ROOT / "ports/fuzix/ripple/include", ROOT / "ports/fuzix/upstream/Library/include",
               ROOT / "runtime/include", ROOT / "ports/fuzix/ripple", ROOT / "ports/graphics/include"]
        flags = ["--target", "native", "-DRIPPLE_MMU=1", "-DFUSE_X_USER=1", "-DMIXER_FAKE_AUDIO=1"]
        flags += [part for path in [*includes, *sdk] for part in ("-I", str(path))]
        objects = []
        for number, unit in enumerate(minimal_units(source)):
            ir = work / (str(number) + "-" + unit.stem + ".ir")
            result = subprocess.run([str(pinned), "compile", str(unit), *flags, "--emit-ir"],
                                    capture_output=True, env=dict(os.environ, RUST_LOG="error"), timeout=120)
            (work / (str(number) + "-build.log")).write_bytes(result.stderr)
            if result.returncode:
                raise RuntimeError(result.stderr.decode(errors="replace")[-7000:])
            ir.write_bytes(result.stdout)
            objects.append(ir)
        output = work / "mixer-fixture.fxe"
        command = [sys.executable, str(ROOT / "fuse-x/cc.py"), str(PORT / "tests/mixer.c"), "--compiler", str(pinned),
                   "--static", "--float-format", "--build", str(work / "sdk"), "-o", str(output), "-D", "MIXER_FAKE_AUDIO=1"]
        for path in includes:
            command += ["-I", str(path)]
        for path in objects:
            command += ["--object-ir", str(path)]
        run(command, work / "link.log", timeout=240)
    return output


def guest_run(args, name, executable, command, capture=False):
    sys.path.insert(0, str(ROOT / "ports/tls/tests"))
    from guest import Guest
    with tempfile.TemporaryDirectory(prefix="sdl-mixer-" + name + "-") as directory:
        work = Path(directory)
        recording = work / "output.wav"
        saved = dict(os.environ)
        guest = None
        try:
            os.environ.update(RCC_NATIVE_AUDIO="off", RCC_NATIVE_NET="off")
            if capture:
                os.environ["RCC_NATIVE_AUDIO_WAV"] = str(recording)
            else:
                os.environ.pop("RCC_NATIVE_AUDIO_WAV", None)
            files = [dict(name="/dev/pcm", mode=0o20666, device=0x500)] if capture or name == "controls" else []
            guest = Guest(work, "native", {"sdl-mixer": executable}, files, blocks=65536)
        except BaseException:
            if (work / "guest.log").exists():
                shutil.copyfile(work / "guest.log", args.output / (name + ".log"))
            raise
        finally:
            os.environ.clear()
            os.environ.update(saved)
        try:
            output = guest.shell("sdl-mixer " + command + "; echo SDL_MIXER_STATUS:$?", seconds=180)
            assert "SDL_MIXER_STATUS:0" in output, output
            assert not any(marker in guest.log for marker in
                           (b"native runtime error", b"panic:", b"SDL_MIXER_FAIL", b"Audio stream error")), \
                guest.log[-8000:].decode(errors="replace")
        finally:
            try:
                guest.close()  # Also checks the disposable root filesystem.
            finally:
                (args.output / (name + ".log")).write_bytes(guest.log)
        if capture:
            retained = args.output / (name + ".wav")
            shutil.copyfile(recording, retained)
            return check_capture(retained, name)
        return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--guest", type=Path, default=ROOT / "build/sdl-mixer/sdl-mixer.fxe")
    parser.add_argument("--compiler", type=Path, default=ROOT / "target/release/rcc")
    parser.add_argument("--output", type=Path, default=ROOT / "build/sdl-mixer-tests")
    parser.add_argument("--host-only", action="store_true")
    parser.add_argument("--deterministic-only", action="store_true", help="host and actual C16 callback tests, without production captures")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", choices=("all", "controls", *CASES), default="all")
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.compiler = args.compiler.resolve()
    args.guest = args.guest.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    report_path = args.output / "result.json"
    report_path.unlink(missing_ok=True)
    report = dict(status="running", captures=[],
                  mixer_sha256=hashlib.sha256((PORT / "mixer/mixer.c").read_bytes()).hexdigest(),
                  fixture_sha256=hashlib.sha256((PORT / "tests/mixer.c").read_bytes()).hexdigest())
    try:
        report["host"] = host_deterministic(args)
        if not args.host_only:
            fixture = guest_deterministic_build(args)
            output = guest_run(args, "deterministic", fixture, "--deterministic")
            match = re.search(r"SDL_MIXER_DETERMINISTIC passed checks=\d+", output)
            assert match and match[0] == report["host"], output
            report["c16"] = match[0]
            if not args.deterministic_only:
                assert args.guest.is_file(), "Build ports/sdl/build.py --app mixer first: " + str(args.guest)
                report["guest_sha256"] = hashlib.sha256(args.guest.read_bytes()).hexdigest()
                if args.case in ("all", "controls"):
                    output = guest_run(args, "controls", args.guest, "--controls")
                    assert "SDL_MIXER_CONTROLS passed" in output, output
                    report["controls"] = "passed"
                for case in CASES if args.case == "all" else (args.case,):
                    if case != "controls":
                        report["captures"].append(guest_run(args, case, args.guest, "--capture " + case, capture=True))
        report["status"] = "passed"
    except BaseException as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        report_path.write_text(json.dumps(report, indent=2) + "\n")
    print("SDL mixer: exact callback samples passed on host logical octet carriers" +
          ("" if args.host_only else " and actual C16") +
          ("; actual guest PCM captures passed" if report["captures"] else ""))


if __name__ == "__main__":
    main()
