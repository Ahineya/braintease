#!/usr/bin/env python3
"""Measure shell-to-frame startup and key-to-frame latency on disposable guest disks.

Build Rift first. Run without concurrent builds/tests for comparable wall times.
The measurements include guest loading, RVM cold/JIT work and terminal output.
"""
import argparse
import json
import statistics
import tempfile
import time
import sys
import ctypes
import struct
from pathlib import Path
from test_os import Guest, ROOT
from test_rift import make_image


def cpu_clock(pid):
    # Own-child CPU time separates actual guest/JIT work from host scheduling
    # delays. Darwin's rusage_info_v0 begins with UUID, user and system Mach
    # ticks. Convert with the host timebase (Apple Silicon ticks are not ns).
    if sys.platform != 'darwin':
        return None
    usage = ctypes.create_string_buffer(96)
    library = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True)
    if library.proc_pid_rusage(pid, 0, ctypes.byref(usage)):
        raise OSError(ctypes.get_errno(), 'reading benchmark child CPU time')
    user, kernel = struct.unpack_from('QQ', usage.raw, 16)
    timebase = (ctypes.c_uint32 * 2)()
    if ctypes.CDLL(None).mach_timebase_info(ctypes.byref(timebase)):
        raise RuntimeError('reading host clock timebase')
    return (user + kernel) * timebase[0] / timebase[1] / 1e9


def measure(system, artifact, document, rvm):
    build = ROOT / ('build/fuzix' if system == 'fuzix' else 'fuse-x/build')
    with tempfile.TemporaryDirectory(prefix='rift-bench-', dir='/tmp') as temporary:
        image = make_image(system, build, artifact, Path(temporary))
        command = [build/'fuse-x', '--run', image] if system == 'fuse-x' else [
            rvm, build/'bios.bin', '--disk', image,
            '--memory', '16384000', '--net', 'off', '--jit', '--guest-ctrl-c', '--timeout', '240']
        guest = Guest(command, system)
        try:
            guest.raw(guest.prompt)
            for rows, columns in [(24, 80), (40, 120)]:
                guest.resize(rows, columns)
                # Let the shell finish handling SIGWINCH before timing a new
                # command; resizing can interrupt its pending terminal read.
                guest.pump()
                started = time.perf_counter()
                cpu_started = cpu_clock(guest.process.pid)
                guest.send(('rift' + (' ' + document if document else '') + '\r').encode())
                guest.wait(lambda: 'F10 Menu' in guest.screen.text() and 'NORMAL' in guest.screen.text()
                           and guest.screen.y == 2 and guest.screen.x == 7, 'first frame', 120)
                startup = time.perf_counter() - started
                cpu_startup = cpu_clock(guest.process.pid)
                cpu_startup = cpu_startup - cpu_started if cpu_started is not None else None
                guest.send(b'i')
                guest.wait(lambda: 'INSERT' in guest.screen.text(), 'insert mode')
                samples = []
                cpu_samples = []
                text = 'abcdefghijkl'
                # Observe the final cursor as well as the changed character,
                # rather than acknowledging a partially emitted frame.
                for index, character in enumerate(text):
                    started = time.perf_counter()
                    cpu_started = cpu_clock(guest.process.pid)
                    guest.send(character.encode())
                    guest.wait(lambda: ''.join(guest.screen.lines[2][7:8+index]) == text[:index+1]
                               and guest.screen.y == 2 and guest.screen.x == 8+index, 'typed frame')
                    samples.append(1000 * (time.perf_counter() - started))
                    cpu_finished = cpu_clock(guest.process.pid)
                    if cpu_started is not None:
                        cpu_samples.append(1000 * (cpu_finished - cpu_started))
                print(json.dumps(dict(system=system, document=document or '[Untitled]', rows=rows, columns=columns,
                      startup_seconds=round(startup, 3), median_key_ms=round(statistics.median(samples), 2),
                      startup_cpu_seconds=round(cpu_startup, 3) if cpu_startup is not None else None,
                      median_key_cpu_ms=round(statistics.median(cpu_samples), 2) if cpu_samples else None,
                      max_key_ms=round(max(samples), 2), samples_ms=[round(n, 2) for n in samples])), flush=True)
                guest.send(b'\x1b')
                guest.visible('NORMAL')
                guest.send(b':')
                guest.visible(': command')
                guest.send(b'qa!\r')
                guest.raw(guest.prompt)
            guest.send(b'shutdown\r')
            guest.process.wait(timeout=30)
            assert guest.process.returncode == 0
        finally:
            guest.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os', choices=['fuzix', 'fuse-x', 'both'], default='both')
    parser.add_argument('--artifacts', type=Path, help='directory containing fuzix/rift.rxe and fuse-x/rift.fxe')
    parser.add_argument('--document', choices=['/tmp/main.rfl'], help='also exercise lexical highlighting on a source file')
    parser.add_argument('--rvm', type=Path, default=ROOT/'target/release/rvm', help='VM binary, for comparing JIT revisions')
    args = parser.parse_args()
    for system in (['fuzix', 'fuse-x'] if args.os == 'both' else [args.os]):
        artifact = (args.artifacts / system / ('rift.rxe' if system == 'fuzix' else 'rift.fxe')) if args.artifacts else (
            ROOT / ('build/fuzix/rift.rxe' if system == 'fuzix' else 'fuse-x/build/apps/rift.fxe'))
        measure(system, artifact.resolve(), args.document, args.rvm.resolve())


if __name__ == '__main__':
    main()
