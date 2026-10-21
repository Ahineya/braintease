#!/usr/bin/env python3
"""Syma highlighting, F8 and F5 on fresh guest disks; no installed image is used."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
from test_os import Guest, ROOT
from test_rift import make_image, F5, F8


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--os',choices=['fuse-x','fuzix'],required=True)
    p.add_argument('--rift',type=Path,required=True)
    p.add_argument('--syma',type=Path,required=True)
    p.add_argument('--build',type=Path,default=ROOT/'ports/syma/build/rift-tests')
    a=p.parse_args();a.build.mkdir(parents=True,exist_ok=True)
    build=ROOT/('fuse-x/build' if a.os=='fuse-x' else 'build/fuzix')
    source='''{Universe
 {Program
  {Effects {Pending {Print out {Text "RIFT_SYMA_OK"}}} {Inbox}}}
 {Rules}}
'''
    with tempfile.TemporaryDirectory(prefix='rift-syma-') as temporary:
        directory=Path(temporary)
        files=[] if a.os=='fuse-x' else [dict(name='/bin/syma',source=str(a.syma.resolve()))]
        extension='fxl' if a.os=='fuse-x' else 'rsl'
        library_root='/lib/fuse-x/' if a.os=='fuse-x' else '/lib/riplix/'
        for library in (build/'lib').glob('*.'+extension+'.[0-9]'):
            files.append(dict(name=library_root+library.name,source=str(library)))
        image=make_image(a.os,build,a.rift.resolve(),directory,applications=['sh','cat','echo'],
            extra_files=files,extra_fixtures={'/tmp/script.syma':source,'/tmp/broken.syma':'{Universe\n',
                '/tmp/demo.symws':'{Worksheet V1 100000 {Cell Code "{Add 20 22}" ""}}'})
        if a.os=='fuse-x':
            shared=directory/'shared';shared.mkdir();shutil.copy2(a.syma,shared/'syma.fxe')
            command=[build/'fuse-x','--run',image,'--share',shared]
            expected_tool='syma: /mnt/host/syma.fxe'
        else:
            command=[ROOT/'target/release/rvm',build/'bios.bin','--disk',image,'--memory','16384000','--net','off','--jit','--timeout','300']
            expected_tool='syma: /bin/syma'
        guest=Guest(command,a.os)
        try:
            guest.raw(guest.prompt);guest.send(b'rift /tmp/script.syma\r');guest.visible('syma');guest.visible('F10 Menu')
            def highlighted():
                row=''.join(guest.screen.lines[2]);at=row.find('Universe')
                return at>=0 and guest.screen.styles[2][at][0]==3
            guest.wait(highlighted,'Syma Universe keyword highlighting')
            guest.send(F8);guest.visible(expected_tool);guest.send(b'\r');guest.visible('NORMAL')
            guest.at=len(guest.log);guest.send(F5);guest.raw(b'\nRIFT_SYMA_OK',seconds=120)
            guest.raw(b'Press Enter to return to Rift');assert not guest.screen.alternate
            guest.send(b'\r');guest.visible('Run finished; wait status 0');assert guest.screen.alternate
            guest.send(b':');guest.visible(': command');guest.send(b'e /tmp/broken.syma\r');guest.visible('/tmp/broken.syma')
            guest.at=len(guest.log);guest.send(F5);guest.raw(b'syma:',seconds=120)
            guest.raw(b'Press Enter to return to Rift');guest.send(b'\r');guest.visible('Run finished; wait status')
            guest.send(b':');guest.visible(': command');guest.send(b'e /tmp/demo.symws\r');guest.visible('/tmp/demo.symws')
            guest.at=len(guest.log);guest.send(F5);guest.raw(b'[1] => 42',seconds=120)
            guest.raw(b'Press Enter to return to Rift');guest.send(b'\r');guest.visible('Run finished; wait status 0')
            guest.send(b':');guest.visible(': command');guest.send(b'qa!\r');guest.raw(guest.prompt)
            guest.send(b'shutdown\r');guest.process.wait(timeout=30);assert guest.process.returncode==0
        finally:
            (a.build/(a.os+'-guest.log')).write_bytes(guest.log);guest.close()
        if a.os=='fuse-x':
            import sys;sys.path.insert(0,str(ROOT/'fuse-x'));from test_support import check_root
            check_root(image)
        else:
            import subprocess;subprocess.run(['python3',str(ROOT/'ports/fuzix/tools/fsck.py'),str(image)],check=True)
    report=dict(status='passed',system=a.os,cases=['Syma detection and highlighting','F8 tool resolution',
        'F5 run','F5 worksheet replay','syntax error return','terminal restoration','clean filesystem'],
        rift_sha256=hashlib.sha256(a.rift.read_bytes()).hexdigest(),syma_sha256=hashlib.sha256(a.syma.read_bytes()).hexdigest())
    (a.build/(a.os+'-report.json')).write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))


if __name__=='__main__':main()
