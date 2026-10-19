#!/usr/bin/env python3
"""Independent PCM vectors, chunk layouts and malformed RIFF files."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/music'


def chunk(tag, data):
    return tag + struct.pack('<I', len(data)) + data + (b'\0' if len(data) & 1 else b'')


def riff(*chunks):
    body = b'WAVE' + b''.join(chunks)
    return b'RIFF' + struct.pack('<I', len(body)) + body


def fixture(rate=44100, channels=2, bits=16, frames=5003, metadata=True):
    samples = [((n * 7919) % 65536) - 32768 for n in range(frames * channels)]
    if bits == 8:
        raw = bytes((value + 32768) // 256 for value in samples)
        samples = [(value - 128) * 256 for value in raw]
    elif bits == 24:
        values = [((n * 130363) % 16777216) - 8388608 for n in range(frames * channels)]
        boundaries = [-8388608, -8388607, -32769, -32768, -257, -256, -255, -1,
                      0, 1, 127, 128, 255, 256, 257, 32767, 32768, 8388351, 8388352, 8388607]
        values[:min(len(values), len(boundaries))] = boundaries[:len(values)]
        raw = b''.join(value.to_bytes(3, 'little', signed=True) for value in values)
        samples = [value // 256 for value in values]
    else:
        raw = struct.pack('<' + 'h' * len(samples), *samples)
    align = channels * bits // 8
    fmt = chunk(b'fmt ', struct.pack('<HHIIHH', 1, channels, rate, rate * align, align, bits))
    data = chunk(b'data', raw)
    return riff(*([chunk(b'JUNK', b'odd'), data, chunk(b'LIST', b'hello'), fmt] if metadata else [fmt, data])), samples


def main():
    with tempfile.TemporaryDirectory(prefix='music-wav-') as temporary:
        directory = Path(temporary)
        flags = ['-std=c99', '-Wall', '-Wextra', '-fsanitize=undefined', '-DO_CLOEXEC=0']
        compiler = 'cc'
        if sys.platform == 'darwin':
            compiler = subprocess.check_output(['xcrun', '--find', 'clang'], text=True).strip()
            flags += ['-isysroot', subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()]
        subprocess.run([compiler, *flags, str(PORT/'wav.c'), str(PORT/'tests/wav_probe.c'), '-o', str(directory/'probe')], check=True)
        count = 0
        for rate in (8000, 44100, 48000, 96000):
            for channels in (1, 2):
                for bits in (8, 16, 24):
                    data, samples = fixture(rate, channels, bits)
                    path = directory/'valid.wav';path.write_bytes(data)
                    result = subprocess.run([str(directory/'probe'), str(path)], capture_output=True, text=True)
                    assert result.returncode == 0, result.stderr + result.stdout
                    checksum = 2166136261
                    for value in samples:checksum = ((checksum ^ (value & 65535)) * 16777619) & 0xffffffff
                    assert list(map(int, result.stdout.split())) == [rate, channels, bits, len(samples)//channels, checksum, samples[0]]
                    count += 1
        valid, _ = fixture(metadata=False)
        malformed = [b'', valid[:11], valid[:-1], valid.replace(b'RIFF', b'RIFX', 1),
                     valid[:16] + struct.pack('<I', 0xffffffff) + valid[20:],
                     riff(chunk(b'data', b'1234')),
                     riff(chunk(b'fmt ', b'1234'), chunk(b'data', b'1234'))]
        for offset, value, form in [(20, 3, 'H'), (22, 3, 'H'), (24, 7999, 'I'), (28, 0, 'I'), (32, 1, 'H'), (34, 32, 'H')]:
            item = bytearray(valid);struct.pack_into('<'+form, item, offset, value);malformed.append(bytes(item))
        malformed.append(riff(chunk(b'fmt ', valid[20:36]), chunk(b'data', b'x')))
        malformed.append(riff(chunk(b'fmt ', valid[20:36]), chunk(b'data', b'1234'), chunk(b'data', b'1234')))
        valid24, _ = fixture(bits=24, metadata=False)
        for data in (b'x', b'xx', b'12345'):
            malformed.append(riff(chunk(b'fmt ', valid24[20:36]), chunk(b'data', data)))
        for data in malformed:
            path=directory/'invalid.wav';path.write_bytes(data)
            result=subprocess.run([str(directory/'probe'), str(path)], capture_output=True)
            assert result.returncode == 1, (result.returncode, result.stdout, result.stderr)
        print(f'WAV: {count} sample-exact formats/layouts and {len(malformed)} invalid files passed')


if __name__ == '__main__':main()
