#!/usr/bin/env python3
"""Unicode TUI, shell and VT throughput on private FUZIX/Fuse-X disks."""
import argparse,fcntl,json,os,statistics,struct,sys,tempfile,time,termios
from pathlib import Path
from test_os import Guest,run,ROOT,PORT
sys.path.insert(0,str(PORT/'tools'))
from test_symlinks import check_disk
from bench_foundations import build_probe as build_plain

def build_probe(build,native,output,source):
    if native:return build_plain(build,native,output,source)
    assembly=output.with_suffix(".asm");obj=output.with_suffix(".pobj")
    run([ROOT/"target/release/rcc","compile",source,"-o",assembly,"--bank-size","64000","-DRIPPLE_MMU=1","-DNO_64BIT",*["-I"+str(p) for p in (PORT/"ripple/include",PORT/"upstream/Library/include",ROOT/"runtime/include",PORT/"ripple")]])
    run([ROOT/"src/ripple-asm/target/release/rasm","assemble",assembly,"-o",obj,"--bank-size","64000","--max-immediate","65535"])
    sdk=json.loads(run(["make","-s","--no-print-directory","-C",PORT,"BUILD="+str(build),"RFL_UNICODE=1","rfl-sdk-info"]))
    run([ROOT/"src/ripple-asm/target/release/rlink",sdk["entry"],obj,*sdk["libraries"],"-f","rxe","--bank-size","64000","-o",output])

def image(system,build,apps,folder):
    files=[dict(name='/init',data='FXN1:init:1\n')] if system=='fuse-x' else [dict(name='/init',host=str(build/'init.rxe'))]
    files += [dict(name='/bin/'+name,host=str(source)) for name,source in apps.items()]
    files += [dict(name='/dev/tty',mode=0o20666,device=0x200),
              dict(name='/etc/termcap',data='ansi|vt100|xterm:co#80:li#24:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:me=\\E[0m:\n')]
    manifest=folder/'files.json';manifest.write_text(json.dumps(files));disk=folder/'disk.img'
    run(['node','--input-type=module','-e',r'''
import fs from 'node:fs';
import {rootImage,atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {fat16Image} from './c-test/examples/rOS/mkfat16.mjs';
import {ptyDevices} from './ports/fuzix/tools/pty.mjs';
const [manifest,disk,build,native]=process.argv.slice(1);
const files=JSON.parse(fs.readFileSync(manifest));
for(const f of files){if(f.host){f.data=fs.readFileSync(f.host);delete f.host;}else if(f.data)f.data=Buffer.from(f.data);}
files.push(...ptyDevices(),{name:'/dev/gfx',mode:0o20666,device:0x400,data:Buffer.alloc(0)});
const root=rootImage(files,16384,256);
atomicWrite(disk,native==='1'?root:fat16Image([{name:'FUZIX.SYS',data:fs.readFileSync(build+'/FUZIX.SYS')},{name:'ROOTFS.IMG',data:root}],{totalSectors:196608,sectorsPerFat:256,sectorsPerCluster:4}));
''',manifest,disk,build,int(system=='fuse-x')]);return disk

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--os',choices=['fuzix','fuse-x','both'],default='both')
    p.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
    p.add_argument('--native-build',type=Path,default=ROOT/'fuse-x/build')
    p.add_argument('--output',type=Path,default=ROOT/'build/unicode/acceptance')
    p.add_argument('--baseline',type=Path,help='original terminal.c, with its original terminal.h beside it')
    p.add_argument('--skip-build',action='store_true')
    p.add_argument('--repetitions',type=int,default=5,help='benchmark samples; zero runs correctness checks only')
    p.add_argument('--rfl',type=Path,default=ROOT/'target/release/rfl')
    a=p.parse_args();assert a.repetitions>=0
    output=a.output.resolve();output.mkdir(parents=True,exist_ok=True);results={}
    for system in (['fuse-x','fuzix'] if a.os=='both' else [a.os]):
        native=system=='fuse-x';build=(a.native_build if native else a.build).resolve()
        work=output/system;work.mkdir(exist_ok=True)
        ext='.fxe' if native else '.rxe';tui=work/('unicode-tui'+ext);bench=work/('unicode-bench'+ext)
        sdk=ROOT/'build/unicode'/('native' if native else 'fuzix')
        if not a.skip_build:
            run(['python3',ROOT/'rfl/tools/build_os.py',ROOT/'rfl/tests/fixtures/unicode_tui.rfl','--os',system,'--build-dir',sdk,'--rfl',a.rfl.resolve(),'-o',tui])
            if native:
                run(['python3',ROOT/'fuse-x/apps.py','sh','--build',sdk])
                run(['python3',ROOT/'fuse-x/cc.py',PORT/'tools/cursescheck.c','--tui-theme','--build',sdk,'-o',work/'cursescheck.fxe'])
            else:
                run(['make','-s','-C',PORT,'-j4','BUILD='+str(sdk),sdk/'sh.rxe',sdk/'cursescheck.rxe'])
            # Use the same compiler/libc for the baseline and Unicode VT.
            if not native:run(['make','-s','-C',PORT,'BUILD='+str(sdk),'RFL_UNICODE=1','rfl-sdk'])
            if a.repetitions:build_probe(sdk,native,bench,ROOT/'ports/graphics/tests/benchmark.c')
        shell=sdk/('apps/sh.fxe' if native else 'sh.rxe')
        apps={'cursescheck':(work/'cursescheck.fxe' if native else sdk/'cursescheck.rxe'),'sh':shell,'echo':build/('apps/echo.fxe' if native else 'echo.rxe'),'unicode-tui':tui}
        if a.repetitions:apps['unicode-bench']=bench
        if a.baseline and a.repetitions:
            baseline=work/('before'+ext);source=work/'baseline.c'
            source.write_text('#define VT_BASELINE '+json.dumps(str(a.baseline.resolve()))+'\n'+(ROOT/'ports/graphics/tests/benchmark.c').read_text())
            if not a.skip_build:build_probe(sdk,native,baseline,source)
            apps['before']=baseline
        with tempfile.TemporaryDirectory(prefix='unicode-guest-') as t:
            folder=Path(t);disk=image(system,build,apps,folder)
            command=[build/'fuse-x','--run',disk] if native else [ROOT/'target/release/rvm',build/'bios.bin','--disk',disk,'--memory','16384000','--net','off','--jit','--timeout','600']
            guest=Guest(command,system)
            try:
                guest.raw(guest.prompt)
                out=guest.shell('cursescheck',seconds=180);assert b'CURSES_OK' in out and b'CURSES_FAILED' not in out,out
                # Quoted/unquoted UTF-8 must survive lexical analysis and argv.
                for command in ['echo Привет_κόσμε','echo "Ж中🙂"',"echo 'café é'"]:
                    expected=command.split(' ',1)[1].strip('"\'').encode()
                    out=guest.shell(command);assert b'\n'+expected+b'\n' in out,out
                # Prompt geometry must count cells too, including after Home.
                original_prompt=guest.prompt;guest.prompt='Ж中> '.encode()
                # Split literals so terminal echo cannot look like a prompt.
                guest.shell("PS1='Ж''中> '")
                guest.send(b'echo ok\x01\x05\r');out=guest.raw(guest.prompt)
                assert b'\033[5C' in out and b'\nok\n' in out,out
                guest.prompt=original_prompt
                original=original_prompt.decode()
                guest.shell("PS1='"+original[:2]+"''"+original[2:]+"'")
                # Grapheme-aware interactive shell movement and deletion.
                guest.send('echo Жé中🙂'.encode()+b'\x1b[D\x7f\x1b[D\x1b[3~\r')
                out=guest.raw(guest.prompt);assert '\nЖ🙂\n'.encode() in out,out
                fcntl.ioctl(guest.master,termios.TIOCSWINSZ,struct.pack('HHHH',24,512,0,0))
                start=len(guest.log);guest.send(b'unicode-tui\r');guest.raw(b'TYPE_UTF8',seconds=120)
                typed='Жé中🙂Ă'
                # Split every UTF-8 encoding over separate host reads.
                for byte in typed.encode():guest.send(bytes([byte]));time.sleep(.015)
                guest.send(b'\x1b[D\x7f\r') # remove emoji, keep U+0102 text distinct from Down
                out=guest.raw(b'UNICODE_TUI_OK',seconds=120)
                assert 'UNICODE_INPUT:Жé中Ă'.encode() in out,out
                guest.raw(guest.prompt)
                stream=bytes(guest.log[start:]);(work/'tui.ansi').write_bytes(stream)
                assert 'Привет Ελληνικά café é 中 🙂'.encode() in stream
                assert b'x'*20+'Ж'.encode() in stream # UTF-8 split across the 512-octet draw scratch
                names=(['unicode-bench']+(['before'] if a.baseline else [])) if a.repetitions else []
                samples={};measurements={name:[] for name in names}
                # Warm both executables, then alternate their measurement order.
                for name in names:
                    for _ in range(2):guest.shell(name,seconds=180)
                import re
                for repetition in range(a.repetitions):
                    for name in (names if repetition%2==0 else names[::-1]):
                        out=guest.shell(name,seconds=180).decode(errors='replace')
                        match=re.search(r'UNICODE_BENCH (\d+) (\d+) (\d+)',out);assert match,out
                        measurements[name].append(tuple(map(int,match.groups())))
                for name,values in measurements.items():
                    samples[name]={'samples':values,'ascii_us':statistics.median(v[0] for v in values),'mixed_us':statistics.median(v[1] for v in values),'cell_c16_words':values[0][2]}
                results[system]=samples;print(system,json.dumps(samples),flush=True)
                guest.send(b'shutdown\r');guest.process.wait(timeout=30);assert guest.process.returncode==0
            finally:
                (work/'guest.log').write_bytes(guest.log);guest.close()
            check_disk(disk,native)
    (output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    print('Unicode: both guest ABIs, quoted shell octets, grapheme editing, scratch splits and column clipping passed')
if __name__=='__main__':main()
