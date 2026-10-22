#!/usr/bin/env python3
"""Launch the staged game and adjacent audio helper through a real host mount.

The disposable root deliberately contains no mixer helper. Only the copy in
/mnt/host can supply audible samples, so this covers the shipping launch path
instead of the installed /bin helper used by the other acceptance fixtures.
Speaker output is muted and normal disk images are never opened.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import tempfile

from audio import capture, click_oracle
from gui import Game, ROOT


def run(share, data, output):
    share, data, output = (Path(path).resolve() for path in (share, data, output))
    binary, helper = share / 'lgeneral.fxe', share / 'sdl-mixerd.fxe'
    for path in (binary, helper, share / 'lgeneral/themes/default/click.wav'):
        if not path.is_file():
            raise FileNotFoundError(path)
    output.mkdir(parents=True, exist_ok=True)
    wav = output / 'lgeneral.wav'
    wav.unlink(missing_ok=True)
    report = dict(
        executable_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
        mixer_helper_sha256=hashlib.sha256(helper.read_bytes()).hexdigest(),
        launch='/mnt/host/lgeneral.fxe',
        helper_installed_in_private_root=False,
        checks=[])
    expected = click_oracle(share / 'lgeneral/themes/default/click.wav')
    with tempfile.TemporaryDirectory(prefix='lgeneral-share-', dir=output) as temporary:
        directory = Path(temporary)
        # Game normally installs a helper found alongside its binary. Give it
        # an isolated binary copy, then launch the staged original explicitly.
        isolated = directory / 'isolated'
        isolated.mkdir()
        alias = isolated / 'alias.fxe'
        shutil.copy2(binary, alias)
        assert not (isolated / 'sdl-mixerd.fxe').exists()
        wrapper = directory / 'kernel-wrapper'
        wrapper.write_text('#!/bin/sh\nexec ' +
                           shlex.quote(str(ROOT / 'fuse-x/build/fuse-x')) +
                           ' "$@" --share ' + shlex.quote(str(share)) + '\n')
        wrapper.chmod(0o755)
        old = dict(os.environ)
        os.environ.update(FUSE_X_TEST_KERNEL=str(wrapper), RCC_NATIVE_AUDIO_WAV=str(wav))
        os.environ.pop('SDL_MIXER_HELPER', None)
        os.environ.pop('LGENERAL_DATA', None)
        try:
            game = Game(alias, data, directory)
        finally:
            os.environ.clear()
            os.environ.update(old)
        try:
            game.g.shell('/mnt/host/lgeneral.fxe &')
            game.g.shell('game=$!')
            game.wait(lambda: b'LGENERAL READY title' in game.g.log,
                      message='staged game title with adjacent helper')
            assert b'Looking up data in: /mnt/host/lgeneral' in game.g.log, game.g.log[-6000:]
            report['checks'].append('staged executable discovers its adjacent data directory')
            game.pause(.3)
            start = len(capture(wav))
            game.menu(3)
            game.wait(lambda: expected in capture(wav)[start:],
                      message='exact click from staged adjacent mixer helper')
            game.screenshot(output, 'campaign-with-staged-helper')
            report['checks'].append('adjacent helper produces original click PCM with no installed helper')
            game.event(input=dict(type='close'))
            game.wait(lambda: not game.window(), message='staged game close')
            result = game.g.shell('wait $game; echo LGENERAL_SHARED_EXIT:$?')
            assert 'LGENERAL_SHARED_EXIT:0' in result, result
            for error in (b'panic:', b'runtime error:', b'assertion failed',
                          b'PCM open failed', b'Compiled without sound'):
                assert error not in game.g.log, game.g.log[-6000:]
            report['checks'].append('staged game closes successfully')
        except BaseException as error:
            report.update(status='failed', error=str(error))
            raise
        finally:
            (output / 'guest.log').write_bytes(game.g.log)
            (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
            game.g.close()
        report['private_filesystem_clean'] = True
    recorded = capture(wav)
    report.update(status='passed', frames=len(recorded) // 4,
                  nonzero_sample_cells=sum(bool(value) for (value,) in struct.iter_unpack('<h', recorded)),
                  sample_sha256=hashlib.sha256(recorded).hexdigest())
    report['checks'].append('private filesystem checks clean after shutdown')
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--share', type=Path, default=ROOT / 'fuse-x-shared')
    parser.add_argument('--data', type=Path, default=ROOT / 'build/lgeneral/data')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/lgeneral-mixer-process/shared')
    args = parser.parse_args()
    print(json.dumps(run(args.share, args.data, args.output), indent=2))
