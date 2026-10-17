#!/usr/bin/env python3
"""Quoted command words must be dequoted before builtin/PATH/exec dispatch."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
COMMANDS = r'''mkdir /quoted
cp /bin/echo '/quoted/with space'
cp /bin/echo '/quoted/with*star'
cp /bin/echo '/quoted/a'"'"'b $literal'
chmod 755 '/quoted/with space' '/quoted/with*star' '/quoted/a'"'"'b $literal'
'echo' QUOTED_NAME
'/bin/echo' SINGLE_ABSOLUTE
"/bin/echo" DOUBLE_ABSOLUTE
command=/bin/echo
"$command" QUOTED_VARIABLE
'/quoted/with space' SPACE_PATH
'/quoted/with*star' STAR_PATH
'/quoted/a'"'"'b $literal' PUNCTUATION_PATH
'cd' /quoted
'./with space' RELATIVE_PATH
cat > '/quoted/script with space' <<'SCRIPT_END'
echo "SCRIPT_ZERO:$0"
echo "SCRIPT_ARGS:$#:$1:$2"
SCRIPT_END
chmod 755 '/quoted/script with space'
'/quoted/script with space' 'one two' '$literal'
'/bin/true'; echo QUOTED_STATUS:$?
'/bin/false'; echo QUOTED_FAILURE:$?
echo UNQUOTED_STILL_WORKS
shutdown
'''
EXPECTED = ['QUOTED_NAME', 'SINGLE_ABSOLUTE', 'DOUBLE_ABSOLUTE', 'QUOTED_VARIABLE',
            'SPACE_PATH', 'STAR_PATH', 'PUNCTUATION_PATH', 'RELATIVE_PATH',
            'SCRIPT_ZERO:/quoted/script with space', 'SCRIPT_ARGS:2:one two:$literal',
            'QUOTED_STATUS:0', 'QUOTED_FAILURE:1', 'UNQUOTED_STILL_WORKS']

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os', choices=('fuzix', 'fuse-x', 'both'), default='both')
    parser.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
    parser.add_argument('--native-build', type=Path, default=ROOT / 'fuse-x/build')
    args = parser.parse_args()
    for system in (['fuse-x', 'fuzix'] if args.os == 'both' else [args.os]):
        with tempfile.TemporaryDirectory(prefix='quoted-shell-') as temporary:
            work = Path(temporary); image = work / 'disk.img'
            script = work / 'quoted-suite'; script.write_text(COMMANDS)
            launch = 'sh /bin/quoted-suite\n'
            if system == 'fuse-x':
                build = args.native_build.resolve()
                subprocess.run(['node', str(ROOT / 'fuse-x/image.mjs'), 'create', str(image),
                                '--apps', str(build / 'apps')], check=True, capture_output=True)
                subprocess.run(['node', str(ROOT / 'fuse-x/image.mjs'), 'put', str(image),
                                '/bin/quoted-suite', str(script)], check=True, capture_output=True)
                command = [str(build / 'fuse-x'), '--run', str(image)]
            else:
                build = args.build.resolve(); stage = work / 'stage'; stage.mkdir()
                names = ['init', 'sh', 'echo', 'cat', 'cp', 'chmod', 'mkdir', 'true', 'false']
                for name in [*(n + '.rxe' for n in names), 'FUZIX.SYS', 'bios.bin']:
                    (stage / name).symlink_to(build / name)
                (stage / 'quoted-suite.rxe').write_text(COMMANDS)
                (stage / 'apps.json').write_text(json.dumps(names + ['quoted-suite']))
                subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(stage), str(image)],
                               check=True, capture_output=True)
                command = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'), '--disk',
                           str(image), '--memory', '16384000', '--headless', '--net', 'off',
                           '--jit', '--input', launch, '--timeout', '90']
            result = subprocess.run(command, input=launch if system == 'fuse-x' else None,
                                    capture_output=True, text=True, timeout=100,
                                    env=dict(os.environ, RCC_NATIVE_HEADLESS='1', RCC_NATIVE_NET='off',
                                             RCC_NATIVE_TIMEOUT='90'))
            output = (result.stdout + result.stderr).replace('\r', '')
            Path('/tmp/shell-paths-' + system + '.log').write_text(output)
            lines = output.splitlines()
            assert result.returncode == 0, output
            for expected in EXPECTED: assert expected in lines, (expected, output)
            assert 'not found' not in output and 'panic:' not in output, output
            print(system + ': quoted builtin/PATH/absolute/relative command words, punctuation, script argv[0] and statuses passed')

if __name__ == '__main__': main()
