#!/usr/bin/env python3
"""Capture actual foreground argv/cwd and restore the default workspace after reboot."""
import argparse
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'ports/tls/tests'))
from guest import Guest
sys.path.insert(0, str(ROOT/'ports/fuzix/tools'))
from test_mux_restore import Terminal


def recipes(text):
    result = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 6 and parts[0] == 'P':
            result[int(parts[1])] = tuple('' if x == '-' else bytes.fromhex(x).decode() for x in parts[4:])
    return result


def run(target):
    build = ROOT/('fuse-x/build' if target == 'native' else 'build/fuzix')
    apps = {name: build/(f'apps/{name}.fxe' if target == 'native' else f'{name}.rxe') for name in ('mux','muxcheck','pwd')}
    files = [dict(name='/work/folder name/README', text='Unicode Ж'), dict(name='/work/idle/README', text='idle')]
    for i in range(16):
        files += [dict(name=f'/dev/ptyp{i:x}',mode=0o20600,device=0x300+i),
                  dict(name=f'/dev/ttyp{i:x}',mode=0o20600,device=0x202+i)]
    with tempfile.TemporaryDirectory(prefix='mux-default-') as temporary:
        directory = Path(temporary)
        def boot(reuse=False): return Guest(directory,target,apps,files,blocks=65536,reuse=reuse)
        guest = boot();term = Terminal(guest)
        try:
            term.line('mux');term.active=True;term.shown('1:shell 1')
            term.line("cd '/work/folder name'; TOKEN=live; muxcheck --workspace 'alpha beta' \"a'b\" 'Ж'")
            term.shown('WORKSPACE_READY');term.shown('WORKSPACE_ARG alpha beta')
            term.key(b'c');term.shown('2:shell 2')
            term.line("PS1=IDLE_'READY> '; cd /work/idle; echo FINISHED_NOT_REPLAYED");term.shown('IDLE_READY> ')
            term.key(b'S');term.shown('Saved main;');term.leave()
            snapshot = guest.shell('cat $HOME/.mux/default')
            r = recipes(snapshot)
            assert r[0] == ('/work/folder name', "'muxcheck' '--workspace' 'alpha beta' 'a'\\''b' 'Ж'; exec '/bin/sh'"), r
            assert r[1] == ('/work/idle',''), r
            assert 'MUX-SESSION 4' in snapshot and 'R 0 0' in snapshot, snapshot
            # Plain mux reattaches to the running server instead of restoring another copy.
            term.line('mux');term.active=True;term.shown('2:shell 2')
            term.key(b'1');term.shown('WORKSPACE_READY');term.line('quit');term.shown('WORKSPACE_EXIT')
            term.line('echo TOKEN_$TOKEN');term.shown('TOKEN_live');term.leave()
            print(target+': exact foreground argv, Unicode, quoted arguments, launch cwd, idle cwd, finished-command exclusion and live attach passed',flush=True)
        finally:
            (ROOT/'build/tui-mouse'/('mux-default-'+target+'-capture.ansi')).write_bytes(guest.log)
            if term.active:term.leave()
            guest.close()
        guest = boot(True);term = Terminal(guest)
        try:
            # No restore subcommand: the default is picked up automatically after a real reboot.
            term.line('mux');term.active=True;term.shown('2:shell 2')
            term.line('pwd');term.shown('/work/idle')
            term.key(b'1');term.shown('WORKSPACE_READY');term.shown('WORKSPACE_ARG Ж')
            term.line('quit');term.shown('WORKSPACE_EXIT')
            term.line('echo RESTORED_SHELL');term.shown('RESTORED_SHELL')
            # Explicit overrides survive saving even with a different foreground app.
            term.configure('echo MANUAL_RECIPE; exec /bin/sh','/work/idle')
            term.line('muxcheck --workspace OTHER');term.shown('WORKSPACE_ARG OTHER')
            term.key(b'S');term.shown('Saved main;');term.leave()
            r = recipes(guest.shell('cat $HOME/.mux/default'))
            assert r[0] == ('/work/idle','echo MANUAL_RECIPE; exec /bin/sh'),r
            guest.shell('mux kill main')
            # Fresh sessions explicitly bypass a valid or invalid default.
            term.line('mux new main');term.active=True;term.shown('1:shell 1')
            term.line('echo FRESH_SHELL');term.shown('FRESH_SHELL',absent='MANUAL_RECIPE')
            # Background launches cannot become the pane's foreground restart command.
            term.line("PS1=BG_'READY> '; muxcheck --workspace BACKGROUND &");term.shown('BG_READY> ');term.shown('WORKSPACE_ARG BACKGROUND')
            term.key(b'S');term.shown('Saved main;');term.leave()
            r = recipes(guest.shell('cat $HOME/.mux/default'));assert not r[0][1],r
            guest.shell('mux kill main')
            guest.shell('echo INVALID > $HOME/.mux/default')
            result=guest.shell('mux');assert 'default workspace is invalid' in result,result
            term.line('mux new main');term.active=True;term.shown('1:shell 1');term.leave()
            guest.shell('mux kill main')
            print(target+': reboot default restore, return-to-shell, manual overrides, fresh-session bypass, background exclusion and invalid-default handling passed',flush=True)
        finally:
            (ROOT/'build/tui-mouse'/('mux-default-'+target+'-restore.ansi')).write_bytes(guest.log)
            if term.active:term.leave()
            guest.close()


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target',choices=['native','ripple','both'],default='native')
    args=p.parse_args()
    for target in (['native','ripple'] if args.target=='both' else [args.target]):run(target)
