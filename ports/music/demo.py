#!/usr/bin/env python3
"""Generate a quiet, original PCM melody for checking guest playback."""
import argparse
import math
from pathlib import Path
import struct
import wave


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    parser.add_argument('--seconds',type=int,default=8)
    args=parser.parse_args()
    if not 1<=args.seconds<=60:parser.error('--seconds must be 1..60')
    rate=44100
    notes=[261.6256,329.6276,391.9954,523.2511,440.0,391.9954,329.6276,261.6256]
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with wave.open(str(args.output),'wb') as wav:
        wav.setparams((2,2,rate,0,'NONE','not compressed'))
        for second in range(args.seconds):
            data=bytearray()
            for frame in range(rate):
                t=frame/rate
                envelope=min(1.0,t*20,(1-t)*10)
                note=notes[second%len(notes)]
                left=int(2400*envelope*(math.sin(2*math.pi*note*t)+.25*math.sin(2*math.pi*note*2*t)))
                right=int(2400*envelope*(math.sin(2*math.pi*note*t)+.25*math.sin(2*math.pi*note*3*t)))
                data.extend(struct.pack('<hh',left,right))
            wav.writeframes(data)
    print(args.output.resolve())


if __name__=='__main__':main()
