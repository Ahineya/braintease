#!/usr/bin/env python3
"""Real Unix drivers and WAV player, captured through the shared host PCM sink."""
import argparse
import os
from pathlib import Path
import struct
import shutil
import sys
import tempfile
import time
import wave
from test_wav import ROOT, fixture

sys.path.insert(0,str(ROOT/'ports/tls/tests'))
from guest import Guest


def check(target, command, expected, rate, channels, source=None, live=False):
    build=ROOT/('fuse-x/build' if target=='native' else 'build/fuzix')
    suffix='.fxe' if target=='native' else '.rxe'
    app=build/('apps' if target=='native' else '')/((command.split()[0])+suffix)
    with tempfile.TemporaryDirectory(prefix='music-guest-') as temporary:
        directory=Path(temporary);capture=directory/'output.wav'
        previous=os.environ.get('RCC_NATIVE_AUDIO_WAV')
        previous_audio=os.environ.get('RCC_NATIVE_AUDIO')
        if live:os.environ['RCC_NATIVE_AUDIO']='on'
        os.environ['RCC_NATIVE_AUDIO_WAV']=str(capture)
        files=[dict(name='/dev/pcm',mode=0o20666,device=0x500)]
        if source:
            host=directory/'input.wav';host.write_bytes(source)
            files.append(dict(name='/track.wav',mode=0o100644,path=str(host)))
        # RVM accepts capture options through a tiny test-only launcher.
        wrapper=directory/'rvm'
        wrapper.write_text('#!/bin/sh\nexec "'+str(ROOT/'target/release/rvm')+'" "$@" --audio '+('on' if live else 'off')+' --audio-wav "'+str(capture)+'"\n')
        wrapper.chmod(0o700)
        old_rvm=os.environ.get('RIPPLE_TEST_RVM');os.environ['RIPPLE_TEST_RVM']=str(wrapper)
        guest=None
        started=time.monotonic()
        try:
            guest=Guest(directory,target,{command.split()[0]:app},files=files,blocks=32768)
            output=guest.shell(command+'; echo MUSIC_STATUS:$?',seconds=120)
            print(output,flush=True)
            assert 'MUSIC_STATUS:0' in output,output
            closed=guest;guest.close();guest=None
            if live:
                assert b'Audio output unavailable' not in closed.log, 'Host speaker could not be opened'
                assert b'Audio stream error' not in closed.log, 'Host speaker stream failed'
        finally:
            if guest:guest.close()
            if previous is None:os.environ.pop('RCC_NATIVE_AUDIO_WAV',None)
            else:os.environ['RCC_NATIVE_AUDIO_WAV']=previous
            if previous_audio is None:os.environ.pop('RCC_NATIVE_AUDIO',None)
            else:os.environ['RCC_NATIVE_AUDIO']=previous_audio
            if old_rvm is None:os.environ.pop('RIPPLE_TEST_RVM',None)
            else:os.environ['RIPPLE_TEST_RVM']=old_rvm
        if '--benchmark' in command:
            return
        retained=ROOT/'build/music-tests';retained.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(capture,retained/f'capture-{target}-{rate}-{channels}.wav')
        with wave.open(str(capture),'rb') as wav:
            assert (wav.getframerate(),wav.getnchannels(),wav.getsampwidth())==(rate,channels,2)
            raw=wav.readframes(wav.getnframes())
            actual=struct.unpack('<'+'h'*(len(raw)//2),raw)
        assert tuple(expected)==actual,(target,len(expected),len(actual),next(((i,a,b) for i,(a,b) in enumerate(zip(expected,actual)) if a!=b),None))
        if command.startswith('wavplay /'):assert '0 missing frames' in output,output
        print(f'{target}: {command.split()[0]}, {rate} Hz/{channels} ch, {len(actual)//channels} sample-exact frames; {time.monotonic()-started:.2f}s including boot/shutdown')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',choices=['native','ripple','both'],default='both')
    parser.add_argument('--pcm-only',action='store_true')
    parser.add_argument('--benchmark',action='store_true')
    parser.add_argument('--benchmark-output',action='store_true')
    parser.add_argument('--seconds',type=int,default=3)
    parser.add_argument('--bits',type=int,choices=[8,16,24],help='test a selected PCM width at 44.1 and 48 kHz stereo')
    parser.add_argument('--live',type=Path,help='play a short, quiet WAV through the real speaker too')
    args=parser.parse_args()
    for target in (['native','ripple'] if args.target=='both' else [args.target]):
        if args.live:
            with wave.open(str(args.live),'rb') as wav:
                rate,channels=wav.getframerate(),wav.getnchannels();raw=wav.readframes(wav.getnframes())
                assert wav.getsampwidth()==2
            check(target,'wavplay /track.wav',struct.unpack('<'+'h'*(len(raw)//2),raw),rate,channels,args.live.read_bytes(),live=True)
            continue
        if args.benchmark or args.benchmark_output:
            source,samples=fixture(44100,2,args.bits or 16,44100*args.seconds)
            check(target,'wavplay '+('--benchmark-output' if args.benchmark_output else '--benchmark')+' /track.wav',samples,44100,2,source)
            continue
        check(target,'pcmcheck',[i*64-32768 for i in range(1024)],8000,1)
        if not args.pcm_only:
            cases=[(8000,1,8,1003),(44100,2,16,44100*args.seconds),(48000,2,16,48000*args.seconds),
                   (8000,1,24,1003),(44100,2,24,44100*args.seconds),(48000,2,24,48000*args.seconds)]
            if args.bits:cases=[case for case in cases if case[2]==args.bits]
            for rate,channels,bits,frames in cases:
                source,samples=fixture(rate,channels,bits,frames)
                check(target,'wavplay /track.wav',samples,rate,channels,source)


if __name__=='__main__':main()
