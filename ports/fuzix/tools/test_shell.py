#!/usr/bin/env python3
"""Boot the upstream Bourne shell, execute scripts, and reboot with the selected RVM engine(s)."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=ROOT/'build/fuzix')
parser.add_argument('--engine', choices=('both', 'jit', 'interpreter'), default='jit', help='RVM engine coverage (default: jit)')
options = parser.parse_args()
engines = [True, False] if options.engine == 'both' else [options.engine == 'jit']
engine_label = 'JIT and interpreter' if options.engine == 'both' else options.engine
build = options.build.resolve()

SCRIPT = """echo ARGS:$#:$1:$2
for x in alpha beta gamma; do echo LOOP:$x; done
if false; then echo WRONG; elif true; then echo IF_OK; else echo WRONG; fi
false || echo OR_OK
true && echo AND_OK
case coffee in tea) echo WRONG;; cof*) echo CASE_OK;; esac
v=unset
until test "$v" = done; do v=done; echo UNTIL_OK; done
while test "$v" = done; do echo WHILE_OK; break; done
set first second
shift
echo SHIFT:$#:$1
eval 'echo EVAL:$1'
echo SUB:`echo substitution`
echo DEFAULT:${missing-default}
RIPPLE_SHELL_TEST=exported
export RIPPLE_SHELL_TEST
printenv RIPPLE_SHELL_TEST
(cd /tmp; pwd)
echo SCRIPT_DONE
"""
COMMANDS = """cat > /shell-suite <<'END_SCRIPT'
""" + SCRIPT + """END_SCRIPT
sh /shell-suite one two
chmod 755 /shell-suite
/shell-suite three four
cat > /shell-env <<'END_ENV'
SOURCED=source_ok
END_ENV
. /shell-env
echo SOURCE:$SOURCED
mkdir /glob
 touch /glob/apple /glob/apricot /glob/banana
echo GLOB: /glob/a*
echo QUESTION: /glob/banan?
echo RANGE: /glob/[ab]*
echo NOMATCH: /glob/nomatch*
echo LITERAL: '/glob/a*'
echo ROOTGLOB: /*/apple
cat > /here <<END_HERE
hello $SOURCED
END_HERE
cat /here
cat > /quoted <<'END_QUOTED'
literal $SOURCED
END_QUOTED
cat /quoted
seq 1 5 | cat | wc -l
false | true; echo PIPE_OK:$?
true | false; echo PIPE_FAIL:$?
sleep 1 &
wait
echo WAIT_OK
sh -c 'exit 9' &
sleep 1
wait $!; echo BACKGROUND_STATUS:$?
trap 'echo TRAP_OK' 10
kill -10 $$
echo AFTER_TRAP
sh -c 'trap "echo EXIT_TRAP" 0; exit 7'; echo CHILD_STATUS:$?
sh -c 'echo SCRIPT_ERROR; if'; echo SYNTAX_STATUS:$?
echo STILL_ALIVE
libccheck
sync
shutdown
"""

def run(disk, commands, jit):
    command = [str(ROOT/'target/release/rvm'), str(build/'bios.bin'), '--disk', str(disk),
               '--memory', '16384000', '--headless', '--input', commands, '--timeout', '120']
    if jit:
        command.append('--jit')
    log = Path(f'/tmp/fuzix-shell-{os.getpid()}-live.log')
    with log.open('w') as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                text=True, timeout=130)
    output = log.read_text().replace('\r', '').split('Starting /init\n')[-1]
    output = '\n'.join(line for line in output.splitlines()
                       if not line.startswith(('fuzix# ', '> ')))
    assert result.returncode == 0 and 'disk synced; power off.' in output, output
    assert 'WRONG' not in output and 'FAILED' not in output and 'panic:' not in output, output
    return output

with tempfile.TemporaryDirectory(prefix='fuzix-shell-') as temporary:
    disk = Path(temporary)/'shell.img'
    subprocess.run(['node', str(PORT/'tools/disk.mjs'), str(build), str(disk)], check=True)
    output = run(disk, COMMANDS, engines[0])
    Path('/tmp/fuzix-shell-test-output.log').write_text(output)
    for expected in ['ARGS:2:one:two', 'ARGS:2:three:four', 'LOOP:alpha', 'LOOP:gamma',
                     'IF_OK', 'OR_OK', 'AND_OK', 'CASE_OK', 'UNTIL_OK', 'WHILE_OK',
                     'SHIFT:1:second', 'EVAL:second', 'SUB:substitution', 'DEFAULT:default',
                     'exported', '/tmp\n', 'SOURCE:source_ok',
                     'GLOB: /glob/apple /glob/apricot', 'QUESTION: /glob/banana',
                     'RANGE: /glob/apple /glob/apricot /glob/banana',
                     'NOMATCH: /glob/nomatch*', 'LITERAL: /glob/a*', 'ROOTGLOB: /glob/apple',
                     'hello source_ok', 'literal $SOURCED', 'WAIT_OK', 'TRAP_OK',
                     'AFTER_TRAP', 'EXIT_TRAP', 'CHILD_STATUS:7', 'SYNTAX_STATUS:2',
                     'PIPE_OK:0', 'PIPE_FAIL:1', 'BACKGROUND_STATUS:9',
                     'STILL_ALIVE', 'libccheck passed']:
        assert expected in output, (expected, output)
    subprocess.run(['python3', str(PORT/'tools/fsck.py'), str(disk)], check=True)
    output = run(disk, 'sh /shell-suite persisted arguments\ncat /here /quoted\nlibccheck\nshutdown\n', engines[-1])
    assert ('ARGS:2:persisted:arguments' in output and
            'literal $SOURCED' in output and 'libccheck passed' in output), output
    subprocess.run(['python3', str(PORT/'tools/fsck.py'), str(disk)], check=True)
print('Bourne shell passed: scripts, control flow, globbing, substitutions, heredocs, traps, wait, recovery and persistence; ' + engine_label)
