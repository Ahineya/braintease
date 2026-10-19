#!/usr/bin/env python3
"""Audit a local minimp3 header and decode a small MP3 inside both Unix guests.

Exploratory build output only: this does not add a production MP3 player.
Fetch the pinned public header, README and license separately; no network here.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def checked(command, log=None):
    result = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True, text=True)
    if log:
        log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(result.stderr[-3000:] + result.stdout[-2000:])
    return result.stdout


def adapt(source):
    # minimp3 assumes 32-bit int, including bitreader positions and sample rate.
    source = re.sub(r'\bunsigned\s+int\b', 'uint32_t', source)
    source = re.sub(r'\bsigned\s+int\b', 'int32_t', source)
    source = re.sub(r'\bunsigned\b(?!\s+(?:char|short|long)\b)', 'uint32_t', source)
    source = re.sub(r'\bint\b', 'int32_t', source)
    # uint8_t promotes to a 16-bit unsigned int on C16. Widen before multiplying.
    source = re.sub(r'\b256u\b', '256UL', source)
    # The wide uint8_t carrier promotes to unsigned int on C16, so subtracting
    # 210 must widen first or a negative gain exponent wraps at 65536.
    source = source.replace('gain_exp = gr->global_gain', 'gain_exp = (int32_t)gr->global_gain')
    # In particular, the dequantizer uses 1 << 30; C16's bare 1 is only 16 bits.
    return re.sub(r'\b(\d+)([uU]?)\s*<<',
                  lambda match: match[1] + ('UL' if match[2] else 'L') + ' <<', source)


def extract_pcm(image, target, destination):
    script = r'''
import fs from 'node:fs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {checkRoot} from './ports/fuzix/tools/fsck.mjs';
import {CELLS,BLOCK} from './ports/fuzix/tools/layout.mjs';
const [image,target,destination]=process.argv.slice(1), disk=openImage(image);
const c=checkRoot(target==='native'?disk:fatFile(disk,'ROOTFS.IMG').data);
if(c.issues.length)throw Error('invalid guest image');
let n=c.nodes.get(1),file;
const cell=at=>c.image.readUInt16LE(n.map.get(Math.floor(at/CELLS))*BLOCK+(at%CELLS)*2);
for(let at=0;at<n.size;at+=32){
 let name='';for(let j=1;j<=30&&cell(at+j);j++)name+=String.fromCharCode(cell(at+j));
 if(name==='pcm.raw'){file=c.nodes.get(cell(at));break;}
}
if(!file||file.size>5000000)throw Error('missing or oversized PCM capture');
n=file;const out=Buffer.alloc(n.size);
for(let i=0;i<n.size;i++){const value=cell(i);if(value>255)throw Error('non-octet PCM');out[i]=value;}
fs.writeFileSync(destination,out);
'''
    checked(['node', '--input-type=module', '-e', script, image, target, destination])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='directory containing minimp3.h and LICENSE')
    parser.add_argument('--mp3', type=Path, required=True, help='small local test MP3')
    parser.add_argument('--frames', type=int, default=12, help='number of decoded MP3 frames, not PCM frames')
    parser.add_argument('--target', choices=['native', 'ripple', 'both'], default='both')
    parser.add_argument('--compiler', type=Path, default=ROOT / 'target/release/rcc')
    args = parser.parse_args()
    if not 1 <= args.frames <= 500:
        parser.error('--frames must be 1..500')
    source = args.source.resolve(strict=True)
    track = args.mp3.resolve(strict=True)
    output = ROOT / 'build/music-tests/mp3-audit'
    output.mkdir(parents=True, exist_ok=True)
    header = (source / 'minimp3.h').read_text()
    (output / 'minimp3.h').write_text(adapt(header))
    (output / 'LICENSE').write_bytes((source / 'LICENSE').read_bytes())
    implementation = '#define MINIMP3_IMPLEMENTATION\n#define MINIMP3_NO_SIMD\n#define MINIMP3_ONLY_MP3\n#include "minimp3.h"\n'
    decoder = output / 'decoder.c'
    decoder.write_text(implementation)
    probe = ROOT / 'ports/music/tests/mp3_probe.c'
    compiler = args.compiler.resolve(strict=True)
    includes = [output, ROOT / 'ports/fuzix/ripple/include',
                ROOT / 'ports/fuzix/upstream/Library/include', ROOT / 'runtime/include']
    flags = ['--bank-size', '64000', '-DRIPPLE_MMU=1']
    for directory in includes:
        flags += ['-I', directory]
    # Reference uses the unmodified upstream scalar decoder and host C types.
    hostcc = 'cc'
    hostflags = []
    if sys.platform == 'darwin':
        hostcc = checked(['xcrun', '--find', 'clang']).strip()
        hostflags = ['-isysroot', checked(['xcrun', '--sdk', 'macosx', '--show-sdk-path']).strip()]
    hostunit = output / 'host-decoder.c'
    hostunit.write_text(implementation.replace('"minimp3.h"', json.dumps(str(source / 'minimp3.h'))))
    host = output / 'mp3probe.host'
    checked([hostcc, *hostflags, '-O2', '-ffp-contract=off', '-I', source, probe, hostunit, '-o', host])
    host_pcm = output / 'host.pcm'
    oracle = checked([host, track, args.frames, host_pcm])
    expected = re.search(r'MP3_PROBE (\d+) (\d+) (\d+) (\d+) (\d+)', oracle)
    if not expected:
        raise RuntimeError(oracle)
    reference = list(map(int, expected.groups()))
    print('Host reference:', oracle.strip(), flush=True)
    report = {'header_sha256': hashlib.sha256(header.encode()).hexdigest(),
              'compiler': str(compiler),
              'adaptations': ['scalar Layer III only', 'explicit 32-bit int and unsigned',
                              'widen byte-cache multiplication, gain subtraction and literal shifts before C16 promotion'],
              'reference': reference, 'guests': {}}
    sys.path.insert(0, str(ROOT / 'ports/tls/tests'))
    from guest import Guest
    all_identical = True
    for target in (['native', 'ripple'] if args.target == 'both' else [args.target]):
        if target == 'native':
            artifact = output / 'mp3probe.fxe'
            checked([sys.executable, ROOT / 'fuse-x/cc.py', probe, decoder, '-I', output,
                     '--compiler', compiler, '-o', artifact], output / 'native.log')
        else:
            build = ROOT / 'build/fuzix'
            sdk = json.loads(checked(['make', '-s', '--no-print-directory', '-C', ROOT / 'ports/fuzix', f'BUILD={build}', 'rfl-sdk-info']))
            floats = json.loads(checked(['make', '-s', '--no-print-directory', '-C', ROOT / 'ports/fuzix', f'BUILD={build}', 'float-sdk-info']))
            softfloat = [path for path in floats['libraries'] if Path(path).name == 'softfloat.pobj']
            for name, unit in [('decoder', decoder), ('probe', probe)]:
                checked([compiler, 'compile', unit, *flags, '-o', output / (name + '.asm')], output / (name + '.log'))
                checked([ROOT / 'src/ripple-asm/target/release/rasm', 'assemble', output / (name + '.asm'),
                         '-o', output / (name + '.pobj'), '--bank-size', '64000', '--max-immediate', '65535'])
            artifact = output / 'mp3probe.rxe'
            checked([ROOT / 'src/ripple-asm/target/release/rlink', sdk['entry'], output / 'decoder.pobj', output / 'probe.pobj',
                     *softfloat, *sdk['libraries'], '-f', 'rxe', '--bank-size', '64000', '-o', artifact], output / 'link.log')
        with tempfile.TemporaryDirectory(prefix='mp3-audit-') as directory:
            guest = Guest(directory, target, {'mp3probe': artifact}, files=[
                dict(name='/probe.mp3', mode=0o100644, path=str(track))])
            try:
                result = guest.shell('mp3probe /probe.mp3 ' + str(args.frames) + ' /pcm.raw; echo MP3_STATUS:$?', seconds=180)
            finally:
                guest.close()
            extract_pcm(guest.image, target, output / (target + '.pcm'))
            (output / (target + '-guest.log')).write_text(result)
        matched = re.search(r'MP3_PROBE (\d+) (\d+) (\d+) (\d+) (\d+)', result)
        if not matched or 'MP3_STATUS:0' not in result:
            raise RuntimeError(result)
        values = list(map(int, matched.groups()))
        identical = values[:3] == reference[:3] and values[4] == reference[4]
        expected_pcm, actual_pcm = host_pcm.read_bytes(), (output / (target + '.pcm')).read_bytes()
        if len(expected_pcm) != len(actual_pcm):
            raise RuntimeError('Guest PCM length differs from the reference')
        identical = identical and actual_pcm == expected_pcm
        expected_samples = struct.unpack('<' + 'h' * (len(expected_pcm) // 2), expected_pcm)
        actual_samples = struct.unpack('<' + 'h' * (len(actual_pcm) // 2), actual_pcm)
        errors = [a-b for a,b in zip(actual_samples, expected_samples)]
        all_identical &= identical
        speed = values[2] * 1000000 / values[0] / max(1, values[3])
        report['guests'][target] = {'rate': values[0], 'channels': values[1], 'pcm_frames': values[2],
                                    'elapsed_us': values[3], 'checksum': values[4], 'matches_host': identical,
                                    'decode_speed': speed, 'max_sample_error': max(map(abs, errors)),
                                    'different_samples': sum(error != 0 for error in errors)}
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f'{target}: {speed:.3f}x real time; exact host PCM checksum: {identical}; max error {max(map(abs, errors))}', flush=True)
    print(output / 'report.json')
    if not all_identical:
        raise RuntimeError('Guest output differs from unmodified host decoder; inspect before using this adaptation')


if __name__ == '__main__':
    main()
