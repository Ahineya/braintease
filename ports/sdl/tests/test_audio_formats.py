#!/usr/bin/env python3
"""Compare pinned SDL audio with portable 8-bit and 16-bit octet carriers."""
import argparse
import difflib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
sys.path.insert(0, str(PORT))
from adapt import config
from audio_adapt import adapt_audio

FORMATS = [0x0008, 0x8008, 0x0010, 0x8010, 0x1010, 0x9010]


def fnv(data):
    value = 2166136261
    for octet in data:
        value = ((value ^ octet) * 16777619) & 0xffffffff
    return value


def expected_cvt(a, b, sc, dc, rate):
    src, dst = FORMATS[a], FORMATS[b]
    width = (src & 255) // 8
    data = [((i * 53 + 19) ^ (i >> 1)) & 255 for i in range(32 * sc * width)]
    # Match SDL's deliberate filter order and quantization. Keep signedness
    # when narrowing, correcting upstream Convert8's lost format flag.
    if width == 2 and dst & 255 == 16 and bool(src & 0x1000) != bool(dst & 0x1000):
        data = [v for i in range(0, len(data), 2) for v in data[i:i+2][::-1]]
        src ^= 0x1000
    if bool(src & 0x8000) != bool(dst & 0x8000):
        sign_at = 0 if width == 1 or src & 0x1000 else 1
        for i in range(sign_at, len(data), width):
            data[i] ^= 128
        src ^= 0x8000
    if width == 2 and dst & 255 == 8:
        data = data[0 if src & 0x1000 else 1::2]
        width = 1
    elif width == 1 and dst & 255 == 16:
        data = [v for octet in data for v in ([octet, 0] if dst & 0x1000 else [0, octet])]
        width = 2
    if sc != dc:
        if dc == 2:
            data = [v for i in range(0, len(data), width) for v in data[i:i+width] * 2]
        else:
            samples = []
            endian = 'big' if dst & 0x1000 else 'little'
            for i in range(0, len(data), width * 2):
                left = int.from_bytes(bytes(data[i:i+width]), endian, signed=bool(dst & 0x8000))
                right = int.from_bytes(bytes(data[i+width:i+2*width]), endian, signed=bool(dst & 0x8000))
                average = int((left + right) / 2)
                samples.extend((average & ((1 << (width * 8)) - 1)).to_bytes(width, endian))
            data = samples
    frame = width * dc
    if rate == 1:
        data = [v for i in range(0, len(data), frame) for v in data[i:i+frame] * 2]
    elif rate == 2:
        data = [v for i in range(0, len(data), frame * 2) for v in data[i:i+frame]]
    return len(data), fnv(data)


def expected_mix(index, volume):
    fmt = FORMATS[index]
    width = (fmt & 255) // 8
    endian = 'big' if fmt & 0x1000 else 'little'
    signed = bool(fmt & 0x8000)
    data = [((i * 31 + 129) ^ (i >> 1)) & 255 for i in range(259)]
    source = [((i * 53 + 71) ^ (i >> 1)) & 255 for i in range(259)]
    if not volume:
        return fnv(data)
    minimum, maximum = -(1 << (width * 8 - 1)), (1 << (width * 8 - 1)) - 1
    for at in range(1, 258 - width + 1, width):
        left = int.from_bytes(bytes(data[at:at+width]), endian, signed=signed)
        right = int.from_bytes(bytes(source[at:at+width]), endian, signed=signed)
        if not signed:
            left += minimum
            right += minimum
        result = min(maximum, max(minimum, left + int(right * volume / 128)))
        if not signed:
            result -= minimum
            if width == 1:
                result = min(254, result)
        data[at:at+width] = (result & ((1 << (width * 8)) - 1)).to_bytes(width, endian)
    return fnv(data)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, default=ROOT / 'build/sdl/audio-formats')
    p.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    p.add_argument('--guest', action='store_true')
    args = p.parse_args()
    build = args.build.resolve(); build.mkdir(parents=True, exist_ok=True)
    (build/'result.json').unlink(missing_ok=True)
    archive = ROOT / 'build/downloads/SDL-1.2.15.tar.gz'
    pin = json.loads((PORT / 'PROVENANCE.json').read_text())
    if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != pin['sha256']:
        raise SystemExit('Missing verified pinned SDL archive; prepare the SDL source first')
    with tarfile.open(archive) as tar:
        tar.extractall(build / 'pristine', filter='data')
    pristine = build / 'pristine/SDL-1.2.15'
    results = {}
    for profile in ['reference', 'octet8', 'octet16']:
        source = build / profile
        shutil.copytree(pristine, source, dirs_exist_ok=True)
        (source / 'include/SDL_config.h').write_text(config(False))
        if profile != 'reference':
            adapt_audio(source, profile == 'octet16')
        if profile == 'octet16':
            path = source / 'include/SDL_stdinc.h'; text = path.read_text()
            text = text.replace('typedef int8_t\t\tSint8;', 'typedef int16_t\t\tSint8;')
            text = text.replace('typedef uint8_t\t\tUint8;', 'typedef uint16_t\t\tUint8;')
            text = text.replace('sizeof(Uint8) == 1', 'sizeof(Uint8) == 2').replace('sizeof(Sint8) == 1', 'sizeof(Sint8) == 2')
            path.write_text(text)
        units = [source / 'src/audio/SDL_audiocvt.c', PORT / 'tests/audio-formats.c']
        flags = []
        if profile == 'reference':
            mixer = source / 'src/audio/SDL_mixer.c'
            text = mixer.read_text().replace('#include "SDL_sysaudio.h"',
                'struct ref_audio { SDL_AudioCVT convert; SDL_AudioSpec spec; };\nextern struct ref_audio *current_audio;')
            mixer.write_text(text)
            units += [mixer, source / 'src/audio/SDL_wave.c']; flags += ['-DPROFILE_REFERENCE=1']
        else:
            units += [PORT / 'c16/audio_formats.c']
        command = [*shlex.split(os.environ.get('CC', 'cc')), '-std=c99', '-O2', '-fno-strict-aliasing', *flags]
        sdk = Path('/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk')
        if sys.platform == 'darwin' and sdk.exists(): command += ['-isysroot', str(sdk)]
        command += ['-I' + str(source / 'include'), '-I' + str(PORT / 'c16'), *map(str, units), '-o', str(build / profile / 'fixtures')]
        compile_result = subprocess.run(command, text=True, capture_output=True, timeout=60)
        (build / (profile + '-build.log')).write_text(compile_result.stdout + compile_result.stderr)
        if compile_result.returncode: raise SystemExit(compile_result.stderr[-6000:])
        r = subprocess.run([str(build / profile / 'fixtures')], text=True, capture_output=True, timeout=60)
        (build / (profile + '.txt')).write_text(r.stdout + r.stderr)
        if r.returncode: raise SystemExit(profile + ': ' + r.stderr[-4000:])
        results[profile] = {tuple(line.split()[:-2] if line.startswith('CVT') else line.split()[:-1]): line.split()[-2:] if line.startswith('CVT') else line.split()[-1:] for line in r.stdout.splitlines()}
    adapter = results['octet8']
    if adapter != results['octet16']: raise SystemExit('8-bit/16-bit logical-carrier result mismatch')
    differences = []
    for key, got in adapter.items():
        if key[0] == 'CVT':
            expected = tuple(map(str, expected_cvt(*map(int, key[1:]))))
            if tuple(got) != expected: raise SystemExit(f'PCM conversion mismatch {key}: {got} != {expected}')
        elif key[0] == 'MIX':
            if int(got[0]) != expected_mix(*map(int, key[1:])):
                raise SystemExit(f'PCM mixing mismatch {key}: {got}')
        elif key[0] == 'WAV':
            sc, depth, freq, fmt, channels, length = map(int, key[1:])
            expected_length = 32 * sc * (2 if depth else 1)
            expected_hash = fnv([((i * 53 + 19) ^ (i >> 1)) & 255 for i in range(expected_length)])
            if (freq, fmt, channels, length, int(got[0])) != (44100, 0x8010 if depth else 8, sc, expected_length, expected_hash):
                raise SystemExit(f'WAV PCM mismatch {key}: {got}')
        reference = results['reference'].get(key)
        if reference is not None and reference != got:
            # SDL loses signedness only on narrowing 16-bit stereo to S8 mono.
            a,b,sc,dc,_ = map(int,key[1:]) if key[0]=='CVT' else (-1,)*5
            if key[0] != 'CVT' or a not in (2,3,4,5) or b != 1 or sc != 2 or dc != 1:
                raise SystemExit(f'Unexpected upstream mismatch {key}: {reference} != {got}')
            differences.append(key)
    print(f'PASS: 432 PCM conversions match independent scalar expectations on 8/16-bit carriers; {len(differences)} documented upstream signed-narrowing corrections.')
    print('PASS: mixing volume/clipping/endian checks, valid WAVs, odd ancillary chunks, every truncated prefix, malformed headers, rate/frame/overflow rejection.')
    summary = dict(conversions=432, carriers=[8,16], upstream_signed_narrowing_differences=len(differences), status='pass')
    if args.guest:
        guest_check(build, args.compiler.resolve())
        summary['guest'] = 'passed'
    (build / 'result.json').write_text(json.dumps(summary, indent=2)+'\n')


def guest_check(build, compiler):
    """Link just the PCM helpers, then execute in a private, fsck-checked disk."""
    from adapt import prepare
    work = build / 'guest'
    work.mkdir(parents=True, exist_ok=True)
    with (work / '.build.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        source = prepare(work, False, True)
        pinned = work / 'rcc-pinned'
        if not pinned.exists() or pinned.read_bytes() != compiler.read_bytes():
            shutil.copy2(compiler, pinned)
        includes = [source/'include', source/'src', PORT/'c16', PORT/'platform']
        sdk = [ROOT/'ports/fuzix/ripple/include', ROOT/'ports/fuzix/upstream/Library/include',
               ROOT/'runtime/include', ROOT/'ports/fuzix/ripple', ROOT/'ports/graphics/include']
        flags = ['--target', 'native', '-DRIPPLE_MMU=1', '-DFUSE_X_USER=1']
        flags += [part for path in [*includes, *sdk] for part in ['-I', str(path)]]
        objects = []
        for unit in [source/'src/audio/SDL_audiocvt.c', PORT/'c16/audio_formats.c']:
            ir = work / (unit.stem + '.ir')
            compiled = subprocess.run([str(pinned), 'compile', str(unit), *flags, '--emit-ir'],
                                      capture_output=True, env=dict(os.environ, RUST_LOG='error'), timeout=120)
            (work/(unit.stem+'-build.log')).write_bytes(compiled.stderr)
            if compiled.returncode: raise RuntimeError(compiled.stderr.decode(errors='replace')[-6000:])
            ir.write_bytes(compiled.stdout);objects.append(ir)
        output = work/'audio-formats.fxe'
        command = [sys.executable, str(ROOT/'fuse-x/cc.py'), str(PORT/'tests/audio-formats.c'),
                   '--compiler', str(pinned), '--static', '--float-format', '--build', str(work/'sdk'), '-o', str(output)]
        for path in includes: command += ['-I', str(path)]
        for path in objects: command += ['--object-ir', str(path)]
        with (work/'link.log').open('w') as log:
            linked = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=180)
        if linked.returncode: raise RuntimeError((work/'link.log').read_text()[-7000:])
    sys.path.insert(0, str(ROOT/'ports/tls/tests'))
    from guest import Guest
    with tempfile.TemporaryDirectory(prefix='sdl-audio-formats-') as directory:
        guest = Guest(directory, 'native', {'audio-formats': output}, blocks=32768)
        try:
            actual = guest.shell('audio-formats; echo SDL_AUDIO_FORMATS_STATUS:$?', seconds=180)
            (work/'run.log').write_text(actual)
            if 'SDL_AUDIO_FORMATS_STATUS:0' not in actual:
                raise AssertionError(actual[-8000:])
            def records(text):
                return [line for line in text.replace('\r','').splitlines()
                        if line.startswith(('CVT ', 'MIX ', 'WAV ', 'CHECKS '))]
            expected = records((build/'octet8.txt').read_text())
            found = records(actual)
            if found != expected:
                difference = '\n'.join(difflib.unified_diff(expected, found, fromfile='host scalar', tofile='C16', lineterm=''))
                (work/'records.diff').write_text(difference+'\n')
                raise AssertionError(difference[:10000])
            (work/'records.diff').unlink(missing_ok=True)
        finally:
            try:
                guest.close()  # Native Guest.close() performs the private root fsck.
            finally:
                (work/'guest.log').write_bytes(guest.log)
    print('PASS: actual C16 CVT/MIX/WAV/check records exactly match host and scalar results; private filesystem clean.')


if __name__ == '__main__': main()
