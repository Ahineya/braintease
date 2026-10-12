#!/usr/bin/env python3
"""Measure a built Doom engine on the unchanged Rust VM and check native pixels.

Build the matching engines first with `build.py engine --target both --start-map`.
This tool never rebuilds engines or runs the Brainfuck backend. Timings use the
production VM runner; optional PC profiles use a separate replay, not timed data.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
sys.path.insert(0, str(PORT))
from disk_image import prepare_disk
from analyze_profile import analyze


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, out, label):
    with (out / (label + '.stdout.log')).open('w') as stdout, (out / (label + '.stderr.log')).open('w') as stderr:
        subprocess.run([str(v) for v in command], cwd=ROOT, stdout=stdout, stderr=stderr,
                       check=True, timeout=900)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=PORT / 'build/ripple/engine/engine.bin')
    parser.add_argument('--native', type=Path, default=PORT / 'build/native/engine/engine')
    parser.add_argument('--objects', type=Path, default=PORT / 'build/ripple/engine')
    parser.add_argument('--archive', action='append', type=Path, default=[])
    parser.add_argument('--wad', type=Path, default=ROOT / 'c-test/examples/doom/doom1.wad')
    parser.add_argument('--input', type=Path, default=PORT / 'tests/gameplay.json')
    parser.add_argument('--output', type=Path, default=PORT / 'build/gameplay-benchmark')
    parser.add_argument('--frames', type=int, default=32)
    parser.add_argument('--mode', choices=('map', 'title'), default='map')
    parser.add_argument('--profile-frame', action='append', type=int, default=[])
    args = parser.parse_args()
    if args.frames < 2 or any(f < 1 or f > args.frames for f in args.profile_frame):
        parser.error('need at least two frames; profile frames must be inside that range')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    args.binary = args.binary.resolve()
    args.native = args.native.resolve()
    args.input = args.input.resolve()
    args.objects = args.objects.resolve()
    events = json.loads(args.input.read_text())
    if any(not isinstance(e['key'], int) for e in events):
        parser.error('native comparison requires numeric USB HID key IDs in the input script')
    native_script = out / 'input.txt'
    native_script.write_text(''.join(f"{e['tick']} {e['key']} {int(e['down'])}\n" for e in events))
    shutil.copyfile(args.input, out / 'input.json')
    binary = out / 'engine.bin'
    native = out / 'engine-native'
    if binary != args.binary:
        shutil.copyfile(args.binary, binary)
    if native != args.native:
        shutil.copy2(args.native, native)
    disk = out / 'disk.wad'
    disk_metadata = prepare_disk(args.wad.resolve(), disk)
    driver = out / 'profile_vm'
    rlib = ROOT / 'target/release/librvm.rlib'
    run(['rustc', '--edition', '2021', '-O', PORT / 'tools/profile_vm.rs',
         '--extern', f'rvm={rlib}', '-L', f'dependency={ROOT / "target/release/deps"}',
         '-o', driver], out, 'build-driver')
    print(f'Measuring {args.frames} frames with the production Rust VM runner', flush=True)
    run([driver, binary, disk, args.input, out / 'timing', args.frames], out, 'timing')
    rows = [json.loads(line) for line in (out / 'timing/frames.jsonl').read_text().splitlines()]
    frame_pattern = r'^ENGINE frame (\d+) tick (\d+) state (\d+) episode (\d+) map (\d+)$'
    guest_frames = re.findall(frame_pattern, (out / 'timing.stdout.log').read_text(), re.M)
    if len(rows) != args.frames or len(guest_frames) != args.frames:
        raise RuntimeError('missing guest frame records')
    checks = []
    for frame in range(1, args.frames + 1):
        raw = out / f'native-{frame}.rgb565'
        run([native, raw, frame, args.mode, native_script], out, f'native-{frame}')
        reference_frames = re.findall(frame_pattern, (out / f'native-{frame}.stdout.log').read_text(), re.M)
        actual = out / f'timing/frame-{frame}.rgb565'
        if raw.read_bytes() != actual.read_bytes() or reference_frames != guest_frames[:frame]:
            expected_bytes, actual_bytes = raw.read_bytes(), actual.read_bytes()
            differing = sum(expected_bytes[i:i + 2] != actual_bytes[i:i + 2]
                            for i in range(0, max(len(expected_bytes), len(actual_bytes)), 2))
            failure = {'success': False, 'failed_frame': frame,
                       'differing_pixels': differing,
                       'expected_raw': str(raw), 'actual_raw': str(actual),
                       'state_matches': reference_frames == guest_frames[:frame],
                       'checks': checks, 'frames': rows,
                       'binary_sha256': digest(binary), 'native_sha256': digest(native),
                       'script_sha256': digest(args.input)}
            (out / 'results.json').write_text(json.dumps(failure, indent=2) + '\n')
            raise RuntimeError(f'frame {frame}: native pixels or state records differ')
        checks.append({'frame': frame, 'raw_sha256': digest(raw), 'state': reference_frames[-1]})
    profiles = []
    if args.profile_frame:
        print(f'Profiling frames {args.profile_frame} in a separate replay', flush=True)
        run([driver, binary, disk, args.input, out / 'profile', max(args.profile_frame),
             *args.profile_frame], out, 'profile')
        for frame in args.profile_frame:
            if (out / f'profile/frame-{frame}.rgb565').read_bytes() != (out / f'timing/frame-{frame}.rgb565').read_bytes():
                raise RuntimeError(f'profile replay diverged at frame {frame}')
            result = analyze(binary, out / f'profile/frame-{frame}.hits.u64', args.objects, args.archive)
            path = out / f'profile-{frame}.json'
            path.write_text(json.dumps(result, indent=2) + '\n')
            profiles.append(str(path))
    if digest(disk) != disk_metadata['sha256']:
        raise RuntimeError('guest changed the WAD image')
    steady = rows[1:]
    median_seconds = statistics.median(row['seconds'] for row in steady)
    result = {'success': True, 'frames': rows, 'checks': checks, 'profiles': profiles,
              'measurement': 'Production runner, unthrottled logical clock, capture at every presented frame; startup frame excluded from steady statistics.',
              'steady_median_seconds': median_seconds, 'steady_fps': 1 / median_seconds,
              'steady_median_instructions': statistics.median(row['instructions'] for row in steady),
              'binary_sha256': digest(binary), 'native_sha256': digest(native),
              'vm_library_sha256': digest(rlib), 'script_sha256': digest(args.input),
              'disk_image': disk_metadata, 'bank_size': 64000, 'memory_cells': 16_384_000, 'seed': 1}
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f"PASS {args.frames} exact native frames; median {median_seconds:.4f}s ({1 / median_seconds:.2f} fps), {result['steady_median_instructions']:,} instructions")


if __name__ == '__main__':
    main()
