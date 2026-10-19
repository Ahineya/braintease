#!/usr/bin/env python3
"""Keep complete build logs while showing concise, live terminal progress."""
import argparse
from collections import deque
import os
from pathlib import Path
import queue
import re
import signal
import subprocess
import sys
import threading
import time

ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')
ACTIVITY = re.compile(
    r'^(?:RCC\s|LINK\s|Compiling\s|Checking\s|Finished\s|Blocking\s|'
    r'Guest compiler\s|Compiler package ready:|Built\s|Fuse-X applications:)|'
    r': (?:compiled|assembled|built|blocked)(?:\s|$)')
FAILURE = re.compile(
    r'\b(?:error(?:\[[^\]]+\])?:|fatal error:|undefined reference|multiple definitions|'
    r'(?:compilation|application link|linker|build|command) (?:has )?failed\b|'
    r'Traceback \(most recent call last\)|make(?:\[\d+\])?: \*\*\*)', re.I)


class BuildInterrupted(BaseException):
    def __init__(self, signum):
        self.signum = signum


def watch(command, logfile, output=None, heartbeat=10, activity_interval=1):
    output = output if output is not None else sys.stderr
    started = time.monotonic()
    shown = started
    activity = 'Starting build'
    active = {}

    def heartbeat_message():
        if active:
            now = time.monotonic()
            tasks = [f'{name}: {phase} ({int(now - start)}s)'
                     for name, (start, phase) in active.items()]
            return 'Still working: ' + '; '.join(tasks)
        return 'Waiting for build output; last update: ' + activity

    def report(message):
        nonlocal shown
        shown = time.monotonic()
        minutes, seconds = divmod(int(shown - started), 60)
        print(f'[build {minutes}:{seconds:02d}] {message}', file=output, flush=True)

    logfile = Path(logfile)
    logfile.parent.mkdir(parents=True, exist_ok=True)
    with logfile.open('w', buffering=1) as log:
        child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, errors='replace', start_new_session=True,
                                 env=dict(os.environ, PYTHONUNBUFFERED='1', CARGO_TERM_COLOR='never'))
        lines = queue.Queue(maxsize=1024)
        recent = deque(maxlen=40)
        failures = []

        def read():
            try:
                for line in child.stdout:
                    lines.put(line)
            finally:
                lines.put(None)

        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        report(activity)
        try:
            while True:
                try:
                    line = lines.get(timeout=max(.01, heartbeat - (time.monotonic() - shown)))
                except queue.Empty:
                    report(heartbeat_message())
                    continue
                if line is None:
                    break
                log.write(line)
                recent.append(line)
                text = ANSI.sub('', line).strip()
                if FAILURE.search(text):
                    if len(failures) < 12 and text[:1000] not in failures:
                        failures.append(text[:1000])
                    report(text[:1000])
                elif text.startswith('Build task started: '):
                    name = text.removeprefix('Build task started: ')
                    active[name] = (time.monotonic(), 'building')
                    if time.monotonic() - shown >= activity_interval:
                        report(f'Building {name}')
                elif text.startswith('Build task progress: '):
                    name, phase = text.removeprefix('Build task progress: ').split(': ', 1)
                    start = active.get(name, (time.monotonic(), ''))[0]
                    active[name] = (start, phase.removeprefix('Build: '))
                    if time.monotonic() - shown >= activity_interval:
                        report(f'{name}: {active[name][1]}')
                elif text.startswith('Build task finished: '):
                    active.pop(text.removeprefix('Build task finished: '), None)
                elif text.startswith('Build: '):
                    activity = text.removeprefix('Build: ')
                    report(activity)
                elif text.startswith(('Host tools:', 'fuzix:', 'fuse-x:')):
                    activity = text
                    report(text)
                elif ACTIVITY.search(text):
                    activity = text[:180]
                    if time.monotonic() - shown >= activity_interval:
                        report(activity)
                # Continuous low-level output should not suppress the heartbeat.
                if time.monotonic() - shown >= heartbeat:
                    report(heartbeat_message())
            result = child.wait()
        except (KeyboardInterrupt, BuildInterrupted) as error:
            signum = error.signum if isinstance(error, BuildInterrupted) else signal.SIGINT
            try:
                os.killpg(child.pid, signum)
            except ProcessLookupError:
                pass
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
            report('Build interrupted; log: ' + str(logfile))
            raise
        finally:
            child.stdout.close()
        if result:
            if failures:
                report('Build errors:')
                for message in failures:
                    print('  ' + message, file=output, flush=True)
            report(f'Build failed (exit {result}); last output:')
            output.writelines(recent)
            print(f'Full log: {logfile}', file=output, flush=True)
        else:
            report('Build complete')
        return result if result >= 0 else 128 - result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('a build command is required after --')

    def interrupted(signum, frame):
        raise BuildInterrupted(signum)

    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, interrupted)
    try:
        return watch(command, args.log)
    except KeyboardInterrupt:
        return 130
    except BuildInterrupted as error:
        return 128 + error.signum


if __name__ == '__main__':
    sys.exit(main())
