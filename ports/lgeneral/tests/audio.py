#!/usr/bin/env python3
"""Exercise original LGeneral effects through a private muted Fuse-X guest."""
import argparse
from array import array
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import wave

from gui import Game, ROOT


def capture(path):
    try:
        with wave.open(str(path), 'rb') as source:
            assert source.getframerate() == 22050 and source.getnchannels() == 2
            assert source.getsampwidth() == 2
            return source.readframes(source.getnframes())
    except (FileNotFoundError, EOFError, wave.Error):
        return b''


def click_oracle(path):
    """The default click is mono S16/22050, at the game's default volume96."""
    with wave.open(str(path), 'rb') as source:
        assert (source.getframerate(), source.getnchannels(), source.getsampwidth()) == (22050, 1, 2)
        raw = source.readframes(source.getnframes())
    samples = array('h'); samples.frombytes(raw)
    if sys.byteorder != 'little': samples.byteswap()
    return b''.join(struct.pack('<hh', int(value * 96 / 128), int(value * 96 / 128))
                    for value in samples)


def run(binary, data, output):
    binary, data, output = (Path(p).resolve() for p in (binary, data, output))
    output.mkdir(parents=True, exist_ok=True)
    wav = output / 'lgeneral.wav'
    wav.unlink(missing_ok=True)
    report = dict(executable_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(), checks=[])
    helper = binary.parent / 'sdl-mixerd.fxe'
    if helper.exists():
        report['mixer_helper_sha256'] = hashlib.sha256(helper.read_bytes()).hexdigest()
    expected = click_oracle(data / 'themes/default/click.wav')
    with tempfile.TemporaryDirectory(prefix='lgeneral-audio-', dir=output) as directory:
        old_capture = os.environ.get('RCC_NATIVE_AUDIO_WAV')
        os.environ['RCC_NATIVE_AUDIO_WAV'] = str(wav)
        try:
            game = Game(binary, data, Path(directory))
        finally:
            if old_capture is None: os.environ.pop('RCC_NATIVE_AUDIO_WAV', None)
            else: os.environ['RCC_NATIVE_AUDIO_WAV'] = old_capture
        try:
            game.g.shell('lgeneral &'); game.g.shell('game=$!')
            game.wait(lambda: b'LGENERAL READY title' in game.g.log, message='sound-enabled title')
            game.pause(.3)
            start = len(capture(wav))
            game.menu(3)
            game.pause(.6)
            game.screenshot(output, 'campaign-with-sound')
            game.wait(lambda: expected in capture(wav)[start:], message='exact original click effect')
            report['checks'].append('original GUI click matches independent PCM/volume oracle in both channels')
            # Cancel, then launch a complete scenario so all movement/attack
            # sound resources are decoded, including tracked.wav at10989Hz.
            game.click(527, 400)
            game.menu(2)
            game.pause(.3)
            x, y = (800 - 44 - 360) // 2, (600 - 300) // 2
            game.click(x + 35, y + 20)
            game.pause(.2)
            game.click(x + 35, y + 41)
            game.pause(.2)
            game.click(x + 309, y + 200)
            game.wait(lambda: b'LGENERAL READY scenario' in game.g.log or game.next_player_screen(),
                      message='scenario with effects')
            if b'LGENERAL READY scenario' not in game.g.log:
                game.key(44)
            game.wait(lambda: b'LGENERAL READY scenario' in game.g.log, message='loaded scenario')
            assert b'LGENERAL SOUND kukgen/tracked.wav ' in game.g.log, 'nonbinary-rate movement sound was not loaded'
            game.screenshot(output, 'scenario-with-sound')
            for bad in (b'Only exact power-of-two', b'Unsupported PCM', b'PCM open failed',
                        b'Compiled without sound', b'runtime error:', b'panic:', b'assertion failed'):
                assert bad not in game.g.log, game.g.log[-6000:]
            report['checks'].append('scenario resources load with sound enabled, including nonbinary-rate WAV conversion')
            game.event(input=dict(type='close'))
            game.wait(lambda: not game.window(), message='game close')
            result = game.g.shell('wait $game; echo LGENERAL_AUDIO_EXIT:$?')
            assert 'LGENERAL_AUDIO_EXIT:0' in result, result
            # Reopen with persisted sound disabled and verify UI clicks remain
            # silent; no full user profile or normal disk is involved.
            game.g.shell('echo @ > /data/home/.lgames/lgeneral.conf')
            game.g.shell('echo sound_on=0 >> /data/home/.lgames/lgeneral.conf')
            mark = len(game.g.log)
            game.g.shell('lgeneral &'); game.g.shell('game=$!')
            game.wait(lambda: b'LGENERAL READY title' in game.g.log[mark:], message='muted title')
            game.pause(.25)
            start = len(capture(wav))
            game.menu(3)
            game.pause(.5)
            muted = capture(wav)[start:]
            assert len(muted) > 1000 and not any(muted), 'persisted mute produced sound or no PCM'
            report['checks'].append('persisted sound_off silences GUI effects')
            game.event(input=dict(type='close'))
            game.wait(lambda: not game.window(), message='muted game close')
            result = game.g.shell('wait $game; echo LGENERAL_AUDIO_EXIT:$?')
            assert 'LGENERAL_AUDIO_EXIT:0' in result, result
            report['status'] = 'passed'
        except BaseException as error:
            report.update(status='failed', error=str(error))
            raise
        finally:
            (output / 'guest.log').write_bytes(game.g.log)
            (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
            game.g.close()
    recorded = capture(wav)
    report.update(frames=len(recorded) // 4,
                  nonzero_sample_cells=sum(1 for (v,) in struct.iter_unpack('<h', recorded) if v),
                  sample_sha256=hashlib.sha256(recorded).hexdigest())
    report['checks'].append('both instances exit successfully and private filesystem checks clean')
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT / 'build/lgeneral/lgeneral.fxe')
    parser.add_argument('--data', type=Path, default=ROOT / 'build/lgeneral/data')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/lgeneral-mixer/audio')
    args = parser.parse_args()
    print(json.dumps(run(args.binary, args.data, args.output), indent=2))
