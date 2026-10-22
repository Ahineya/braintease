#!/usr/bin/env python3
"""Verify real Graphite apps group by identity on disposable offline disks."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('desktop_group_fixture', ROOT / 'fuse-x/test_desktop_mode.py')
desktop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(desktop)


class ApplicationBuild:
    """Use existing guest tools/libraries with an explicitly selected terminal."""
    def __init__(self, base, terminal):
        self.base, self.terminal = base, terminal

    def __fspath__(self):
        return str(self.base)

    def __truediv__(self, name):
        return self.terminal if name == 'apps/gterm.fxe' else self.base / name


class RegisteredDesktop(desktop.DesktopGUI):
    def __init__(self, output, args, nested):
        build = ApplicationBuild(args.build, args.terminal)
        super().__init__(output, args.kernel, build, args.calculator, nested)

    def group(self, application):
        return next((g for g in self.desktop().get('dock', {}).get('groups', [])
                     if g['app_id'] == application), None)

    def menu(self, application):
        group = self.wait(lambda: self.group(application))
        x, y, w, h = group['rect']
        self.click(0, x + w // 2, y + h // 2)
        return self.wait(lambda: self.desktop().get('dock', {}).get('menu'))

    def choose(self, entry):
        x, y, w, h = entry['rect']
        self.click(0, x + w // 2, y + h // 2)
        self.wait(lambda: not self.desktop()['dock']['menu'])
        self.wait(lambda: self.desktop()['focused'] == entry['window'])


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def launch(ui, command, marker):
    ui.g.shell(command + ' &')
    output = ui.g.shell('echo ' + marker + ':$!')
    match = re.search(r'\b' + marker + r':(\d+)', output)
    assert match, output
    return int(match[1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=ROOT / 'fuse-x/build')
    parser.add_argument('--calculator', type=Path, default=ROOT / 'build/lvgl/dock-identity/lvcalc.fxe')
    parser.add_argument('--terminal', type=Path, default=ROOT / 'build/fusede-dock/gterm.fxe')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/fusede-dock/real-app-tests')
    args = parser.parse_args()
    for name in ('kernel', 'build', 'calculator', 'terminal', 'output'):
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    receipt = args.output / 'result.json'
    receipt.unlink(missing_ok=True)
    report = dict(status='running', kernel_sha256=sha(args.kernel),
                  calculator_sha256=sha(args.calculator), terminal_sha256=sha(args.terminal),
                  test_sha256=sha(Path(__file__)), private_images_only=True, checks=[])
    started = time.monotonic()
    try:
        native = RegisteredDesktop(args.output / 'native', args, False)
        try:
            launch(native, 'LVGL_SCALE=1 lvcalc --trace', 'NATIVE_CALCULATOR')
            wid = native.wait(lambda: native.window('Calculator'))['id']
            reference = native.source(wid)
            assert not (native.frames / 'desktop.json').exists()
            native.text(wid, 'c6*7')
            at = len(native.g.log)
            native.key(wid, 40)
            native.logged('CALC_RESULT 42', at)
            native.send(wid, close=True)
            native.wait(lambda: not native.windows())
            report['checks'].append('native mode unchanged, registered calculator input and close')
        finally:
            native.close()

        ui = RegisteredDesktop(args.output / 'nested', args, True)
        try:
            calculator_pids = [launch(ui, 'LVGL_SCALE=1 lvcalc --trace', 'CALCULATOR_ONE')]
            first = ui.wait(lambda: ui.window('Calculator'))['id']
            assert ui.source(first) == reference, 'Native/nested calculator pixels differ'
            calculator_pids.append(launch(ui, 'LVGL_SCALE=1 lvcalc --trace', 'CALCULATOR_TWO'))
            ui.wait(lambda: len([w for w in ui.windows() if w['title'] == 'Calculator']) == 2)
            calculators = sorted(w['id'] for w in ui.windows() if w['title'] == 'Calculator')
            terminal_pids = []
            for number in (1, 2):
                command = ("gterm -g 40x12 /bin/sh -c 'echo $$ > /tmp/terminal-" +
                           str(number) + ".pid; exec /bin/sh'")
                terminal_pids.append(launch(ui, command, 'TERMINAL_' + str(number)))
                ui.wait(lambda: len([w for w in ui.windows() if w['title'] == 'Terminal']) == number)
            terminals = sorted(w['id'] for w in ui.windows() if w['title'] == 'Terminal')
            for wid in terminals:
                ui.source(wid, 320, 192)
            helpers = []
            for number in (1, 2):
                text = ui.g.shell('cat /tmp/terminal-' + str(number) + '.pid')
                match = re.search(r'(?:^|\n)(\d+)(?:\n|$)', text)
                assert match, text
                helpers.append(int(match[1]))
            assert len(set(calculator_pids + terminal_pids + helpers)) == 6
            calc_group = ui.wait(lambda: ui.group('org.riplix.Calculator'))
            term_group = ui.wait(lambda: ui.group('org.riplix.Terminal'))
            assert set(calc_group['windows']) == set(calculators)
            assert set(term_group['windows']) == set(terminals)
            assert (calc_group['name'], calc_group['icon']) == ('Calculator', 'calculator')
            assert (term_group['name'], term_group['icon']) == ('Terminal', 'terminal')
            assert len(ui.desktop()['dock']['groups']) == 2, 'Helpers generated extra dock entries'
            assert len(ui.desktop()['windows']) == len(ui.windows()) == 4
            assert {w['pid'] for w in ui.windows()} == set(calculator_pids + terminal_pids)
            assert not set(helpers) & {w['pid'] for w in ui.windows()}
            assert len({w['process'] for w in ui.windows()}) == 4
            report['checks'] += ['two independent calculators share one app icon',
                                 'two independent terminals share one app icon',
                                 'actual terminal shell children have no dock entries',
                                 'native and nested initial calculator pixels match']

            # Group menus enumerate actual application windows, including
            # identical titles, and activate the exact selected window.
            for application, windows, label in [
                ('org.riplix.Calculator', calculators, 'calculator-window-menu'),
                ('org.riplix.Terminal', terminals, 'terminal-window-menu'),
            ]:
                menu = ui.menu(application)
                entries = [e for e in menu['entries'] if e['command'] == 'activate']
                assert {e['window'] for e in entries} == set(windows)
                assert len({e['title'] for e in entries}) == 2
                ui.save_scene(label)
                ui.choose(next(e for e in entries if e['window'] == windows[0]))
            report['checks'].append('group menus list only actual windows and disambiguate duplicate titles')

            # Arrange nonoverlapping clients, then compare their published
            # source pixels with the production desktop compositor output.
            placements = [(terminals[0], 24, 50, 320, 192),
                          (terminals[1], 24, 300, 320, 192),
                          (calculators[0], 480, 50, 300, 360),
                          (calculators[1], 810, 50, 300, 360)]
            for wid, x, y, width, height in placements:
                ui.restore(wid)
                ui.move(wid, x, y)
            compared = sum(ui.source_matches_desktop(wid, width, height)
                           for wid, x, y, width, height in placements)
            ui.save_scene('grouped-real-apps')
            menu = ui.menu('org.riplix.Calculator')
            ui.save_scene('grouped-calculator-menu')
            ui.choose(next(e for e in menu['entries']
                           if e['window'] == calculators[0] and e['command'] == 'activate'))
            ui.calculate('6*7', '42')
            report['compared_client_pixels'] = compared
            report['checks'] += ['four real app client frames match desktop pixels',
                                 'calculator receives keyboard input after group-menu activation']
            report['helper_pids'] = helpers
            report['window_ids'] = dict(calculator=calculators, terminal=terminals)
            for wid in calculators + terminals:
                ui.send(wid, close=True)
            ui.wait(lambda: not ui.windows())
            ui.wait(lambda: not ui.desktop()['dock']['groups'])
            report['checks'].append('last application window removal cleans up dock groups')
            assert not any(error in ui.g.log for error in
                           (b'panic:', b'native runtime error', b'Assertion `', b'graphics operation failed'))
        finally:
            ui.close()
        report['status'] = 'passed'
        print('FuseDE real apps: two grouped calculators, two grouped terminals, helpers excluded, pixels passed')
    except BaseException as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        report['elapsed_seconds'] = round(time.monotonic() - started, 3)
        receipt.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
