#!/usr/bin/env python3
"""Drive the real curses worksheet on the host or fresh, offline guest disks."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pty
import re
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time
import tty

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
sys.path.insert(0, str(ROOT / 'rfl/tools'))
from test_os import Screen
from test_rift import F1, F2, F3, F4, F5, F6, F7, F8
from worksheet import document
UP, DOWN, LEFT, RIGHT = [b'\x1b[' + c for c in (b'A', b'B', b'D', b'C')]
HOME, END = b'\x1b[H', b'\x1b[F'


class Terminal:
    def __init__(self, master, process, directory, log=None):
        self.master, self.process, self.directory = master, process, directory
        self.log = bytearray() if log is None else log
        self.screen = Screen()
        self.resize(80, 30)

    def resize(self, columns, rows):
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack('HHHH', rows, columns, 0, 0))
        self.screen.rows, self.screen.columns = rows, columns
        self.screen.lines = [list(' ' * columns) for _ in range(rows)]
        self.screen.styles = [[self.screen.style] * columns for _ in range(rows)]

    def pump(self):
        if select.select([self.master], [], [], .02)[0]:
            try: data = os.read(self.master, 65536)
            except OSError: data = b''
            self.log.extend(data); self.screen.feed(data)

    def wait(self, predicate, description, seconds=35):
        until = time.monotonic() + seconds
        while not predicate():
            self.pump()
            if self.process.poll() is not None or time.monotonic() > until:
                raise AssertionError(description + '\n' + self.screen.text() + '\n' + self.log[-1200:].decode(errors='replace'))
        # A matching label can arrive before the bottom of the same frame.
        until = time.monotonic() + .08
        while time.monotonic() < until: self.pump()

    def visible(self, text, seconds=35):
        self.wait(lambda: text in self.screen.text(), 'missing ' + text, seconds)

    def send(self, data):
        os.write(self.master, data)

    def shot(self, name):
        (self.directory / (name + '.txt')).write_text(self.screen.text())
        (self.directory / (name + '.json')).write_text(json.dumps(dict(
            lines=self.screen.lines, styles=self.screen.styles), ensure_ascii=False))

    def select_cell(self, number):
        self.send(LEFT + HOME + DOWN * (number - 1))
        self.visible('Cell ' + str(number) + ' /')

    def result(self, value):
        self.wait(lambda: any(''.join(row[27:]).strip() == value
                              for row in self.screen.lines[4:27]), 'result ' + value)

    def run(self, key, message, result=None):
        before = len(self.log)
        self.send(key)
        self.wait(lambda: len(self.log) > before, 'run output')
        self.visible(message)
        if result is not None: self.result(result)
        delta = bytes(self.log[before:])
        assert not re.search(rb'\x1b\[(?:[23])?J', delta), 'run cleared the physical screen'
        return delta


def fixtures(base):
    return {
        base + '/ui.symws': document([
            ('Text', 'Objects and algebra. Select cells with the arrows; Enter edits and F5 runs.'),
            ('Code', '{Import Math/Small open}'),
            ('Code', '{Define $p {Sum {Power x 3} {Product 2 x}}}'),
            ('Code', '{Define $d {Differentiate $p x}}'),
            ('Code', '{Substitute $d x 3}'),
            ('Text', 'An ending note.')]),
        base + '/demo.symws': (PORT / 'examples/worksheets/objects-and-math.symws').read_text(),
        base + '/bad.symws': '{Worksheet V1 100 {Cell Code 42 ""}}',
        base + '/session.symws': document([
            ('Code', '{Define $counter 0}'),
            ('Code', '{Define $counter {Add $counter 1}}'),
            ('Code', '$counter'),
            ('Code', '{Broken'),
            ('Code', '{Add 1 2}')]),
        '/etc/termcap': r'ansi|vt100|xterm|xterm-256color|fuse-x:co#80:li#30:cl=\E[H\E[2J:cm=\E[%i%d;%dH:me=\E[0m:vi=\E[?25l:ve=\E[?25h:' + '\n',
    }


def exercise(t, base):
    t.visible('Not run'); t.visible('F5 Run all')
    t.shot('initial'); print('UI: browse and run', flush=True)
    t.run(F5, 'Worksheet evaluated.')
    t.visible('Cell 1 / 6')  # Run all leaves the reading pane where it was.
    # Repeated single-cell evaluation must retain state, not replay the prefix.
    t.send(F3); t.visible('Save worksheet changes'); t.send(b'd'); t.visible('Open worksheet')
    t.send(b'\x15' + (base + '/session.symws').encode() + b'\r'); t.visible('session.symws')
    t.run(F6, 'Cell 1 evaluated.', '0')
    t.select_cell(2); t.run(F6, 'Cell 2 evaluated.', '1')
    delta = t.run(F6, 'Cell 2 evaluated.', '2')
    assert len(delta) < 2000, 'single-cell run repainted unrelated screen content'
    assert b'F1 Help' not in delta and b'CELLS' not in delta
    t.select_cell(3); t.run(F6, 'Cell 3 evaluated.', '2')
    # Add a new cell after a live session; later malformed cells must not block it.
    t.send(b'a'); t.visible('New code cell'); t.send(b'{Add $counter 40}' + F2)
    t.visible('Cell 4 / 6'); t.run(F6, 'Cell 4 evaluated.', '42')
    t.visible('1 Code  Ready'); t.visible('2 Code  Ready')
    # Updating the budget must preserve the live counter and imported definitions.
    t.send(F8); t.visible('Evaluation limit'); t.send(b'\x155000\r'); t.visible('limit updated')
    t.select_cell(3); t.run(F6, 'Cell 3 evaluated.', '2')
    t.run(F5, 'A cell has an error.', '1')  # A full run resets the counter.
    t.visible('Cell 3 / 6'); t.shot('single-cell-session')
    t.send(F3); t.visible('Save worksheet changes'); t.send(b'd'); t.visible('Open worksheet')
    t.send(b'\x15' + (base + '/ui.symws').encode() + b'\r'); t.visible('ui.symws')
    t.run(F5, 'Worksheet evaluated.')
    t.select_cell(4); t.visible('3 * x^2 + 2'); t.shot('mathematics')
    t.send(F7); t.visible('Result  [raw]'); t.visible('Math/Small/Product')
    t.send(F7); t.visible('Readable result')
    t.select_cell(3); t.send(b'\r'); t.visible('Editing code cell')
    t.send(b'\x01{Define $p {Sum {Power x 4} {Product 2 x}}}')
    t.visible('Power x 4')
    t.send(b'\x1a'); t.visible('Power x 3')
    t.send(b'\x19'); t.visible('Power x 4')
    t.send(b'\x01\x10'); t.send(F2); t.visible('Cell updated.'); t.visible('Needs update')
    t.select_cell(5); t.visible('Previous result (needs update)'); t.visible('29')
    t.run(F6, 'Cell 5 evaluated.', '29')  # No implicit rerun of the edited polynomial.
    t.select_cell(3); t.run(F6, 'Cell 3 evaluated.')
    t.select_cell(4); t.run(F6, 'Cell 4 evaluated.', '4 * x^3 + 2')
    t.select_cell(5); t.run(F6, 'Cell 5 evaluated.', '110')
    # A page-sized Unicode paste is batched, wrapped, undoable and preserved on disk.
    print('UI: paste and edit', flush=True)
    t.send(b't'); t.visible('New note cell')
    note = 'Привіт ĀĂЖ é中🙂\n' + ('Long paragraphs should wrap without losing text. ' * 150) + '\nPASTE_END_MARKER Жé中🙂'
    before = len(t.log); start = time.monotonic()
    t.send(b'\x1b[200~' + note.encode() + b'\x1b[201~')
    t.visible('PASTE_END_MARKER', seconds=120)
    paste_seconds = time.monotonic() - start
    assert len(t.log) - before < 200000, 'per-character redraw during paste'
    t.send(b'\x1a'); t.visible('Line 1  Column 1')
    t.send(b'\x19'); t.visible('PASTE_END_MARKER')
    # Backspace removes emoji, CJK and a combining sequence as whole graphemes.
    t.send(b'\x7f\x7f\x7f'); t.visible('PASTE_END_MARKER Ж')
    t.send(F2); t.visible('Cell 6 / 7'); t.visible('Note')
    t.send(RIGHT + END); t.visible('PASTE_END_MARKER'); t.shot('wrapped-note')
    t.send(LEFT + b'['); t.visible('Cell 5 / 7'); t.send(b']'); t.visible('Cell 6 / 7')
    t.send(b'd'); t.visible('Remove the selected cell?'); t.send(b'n'); t.visible('Cell 6 / 7')
    t.send(b'a'); t.visible('New code cell'); t.send(b'{Broken'); t.send(F2)
    t.visible('Cell 7 / 8'); t.run(F6, 'A cell has an error.')
    t.shot('cell-error')
    t.send(b'\r'); t.visible('Editing code cell'); t.send(b'\x01{Add 20 22}' + F5)
    t.visible('Cell 7 evaluated.'); t.result('42')
    print('UI: settings, files and resize', flush=True)
    t.send(F8); t.visible('Evaluation limit'); t.send(b'\x150\r'); t.visible('Enter a limit')
    t.send(F8); t.visible('Evaluation limit'); t.send(b'\x1525000\r'); t.visible('limit updated')
    t.send(F4); t.visible('Save worksheet as'); t.send(b'\x15' + (base + '/ui-saved.symws').encode() + b'\r')
    t.visible('Worksheet saved.')
    t.send(F3); t.visible('Open worksheet'); t.send(b'\x15' + (base + '/bad.symws').encode() + b'\r')
    t.visible('Cell needs Code|Text'); t.visible('ui-saved.symws'); t.visible('Cell 7 / 8')
    t.send(F1); t.visible('SYMA WORKSHEET'); t.send(b'\x1b'); t.visible('F5 Run all')
    t.resize(68, 20); t.visible('F5 Run all'); t.visible('Cell 7 / 8')
    t.resize(100, 32); t.visible('Cell 7 / 8')
    t.select_cell(6); t.send(RIGHT + END); t.visible('PASTE_END_MARKER')
    t.resize(80, 30); t.visible('Cell 6 / 8'); t.visible('PASTE_END_MARKER')
    # Mouse selection uses the same cell model, without invoking an editor.
    print('UI: mouse and closing', flush=True)
    t.send(b'\x1b[<0;3;4M'); t.visible('Cell 1 / 8')
    t.send(b'a'); t.visible('New code cell'); t.send(b'{Add 1 2}' + F2); t.visible('Cell updated.')
    t.send(b'q'); t.visible('Save worksheet changes'); t.send(b'\x1b'); t.visible('F5 Run all')
    t.send(b'\x13'); t.visible('Worksheet saved.'); t.shot('saved')
    t.send(b'q'); t.wait(lambda: not t.screen.alternate, 'restore primary terminal')
    return dict(paste_seconds=round(paste_seconds, 3))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('target', choices=['host', 'native', 'ripple'])
    p.add_argument('--binary', type=Path)
    a = p.parse_args()
    folder, name = {'host': ('host', 'syma'), 'native': ('fuse-x', 'syma.fxe'), 'ripple': ('fuzix', 'syma.rxe')}[a.target]
    binary = (a.binary or PORT / 'build' / folder / name).resolve()
    directory = Path(tempfile.mkdtemp(prefix='tui-' + a.target + '-', dir=PORT / 'build'))
    base = str(directory) if a.target == 'host' else '/tmp'
    data = fixtures(base); start = time.monotonic(); vm = None
    if a.target == 'host':
        for path, source in data.items():
            if path != '/etc/termcap': Path(path).write_text(source)
        master, slave = pty.openpty(); tty.setraw(slave)
        original = termios.tcgetattr(slave)
        process = subprocess.Popen([binary, 'worksheet', base + '/ui.symws'], stdin=slave, stdout=slave, stderr=slave,
            env=os.environ | {'TERM': 'ansi', 'SYMA_PATH': str(PORT / 'library'), 'HOME': str(directory)})
        os.close(slave); terminal = Terminal(master, process, directory)
    else:
        sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
        from guest import Guest
        entries = [dict(name=name, text=source, mode=0o100644) for name, source in data.items()]
        entries += [dict(name='/usr/share/syma/library/' + path.name, path=str(path), mode=0o100644)
                    for path in (PORT / 'library').glob('*.syma')]
        vm = Guest(directory, a.target, {'syma': binary}, entries, blocks=65536, timeout=2400)
        terminal = Terminal(vm.master, vm.process, directory, vm.log)
        terminal.send(b'syma worksheet /tmp/ui.symws\r')
    try:
        result = exercise(terminal, base)
        assert b'\x1b[?2004l' in terminal.log and b'\x1b[?25h' in terminal.log
        if a.target == 'host':
            assert terminal.process.wait(timeout=10) == 0
            assert termios.tcgetattr(terminal.master) == original
            saved = Path(base + '/ui-saved.symws').read_text()
            assert '25000' in saved and 'Привіт ĀĂЖ' in saved and 'PASTE_END_MARKER Ж"' in saved
            r = subprocess.run([binary, 'worksheet', base + '/ui-saved.symws', '--run'], capture_output=True, text=True,
                               env=os.environ | {'SYMA_PATH': str(PORT / 'library')}, timeout=30)
            assert r.returncode == 0 and '=> 110' in r.stdout and '=> 42' in r.stdout, r.stdout + r.stderr
        else:
            vm.at = len(vm.log)
            text = vm.shell('syma worksheet /tmp/ui-saved.symws --run; echo UI_STATUS:$?', 300)
            assert 'UI_STATUS:0' in text and '=> 110' in text and '=> 42' in text, text
            saved = vm.shell('cat /tmp/ui-saved.symws')
            assert 'Привіт ĀĂЖ' in saved and 'PASTE_END_MARKER Ж"' in saved and 'V1 25000' in saved
            if a.target == 'native':
                terminal.send(b'syma worksheet /tmp/demo.symws\r')
                terminal.visible('demo.symws')
                terminal.send(F5); terminal.visible('Worksheet evaluated.', 120)
                terminal.select_cell(8); terminal.visible('3 * x^2 + 2')
                terminal.shot('objects-and-math')
                terminal.select_cell(5); terminal.send(RIGHT + END)
                terminal.visible('["woof", true, false, true, false]')
                terminal.send(b'q'); terminal.visible('Save worksheet changes'); terminal.send(b'd')
                terminal.wait(lambda: not terminal.screen.alternate, 'sample terminal restoration')

    finally:
        (directory / 'terminal.log').write_bytes(terminal.log)
        if vm:
            vm.at = len(vm.log); vm.close()
        else:
            if terminal.process.poll() is None: terminal.process.kill(); terminal.process.wait()
            os.close(terminal.master)
    report = dict(target=a.target, status='passed', seconds=round(time.monotonic() - start, 3),
                  paste_seconds=result['paste_seconds'], sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), logs=str(directory))
    (PORT / 'build' / ('tui-' + a.target + '-result.json')).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__': main()
