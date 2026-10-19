#!/usr/bin/env python3
"""Opt-in Internet DNS/HTTP smoke test through Riplix's default shared vmnet link.

Uses a private disk. The existing macOS helper needs sudo authorization.
Makes DNS queries and one cleartext HTTP download from example.com.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
from launch import host_command
ROOT=Path(__file__).resolve().parents[3]
PORT=ROOT/'ports/fuzix'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
a=p.parse_args();build=a.build.resolve()
with tempfile.TemporaryDirectory(prefix='fuzix-internet-') as directory:
    tmp=Path(directory)
    names=['init','sh','echo','cat','wc','grep','host','dig','htget','ifconfig']
    for file in [*(n+'.rxe' for n in names),'bios.bin','FUZIX.SYS']:(tmp/file).symlink_to(build/file)
    (tmp/'apps.json').write_text(json.dumps(names));image=tmp/'internet.img'
    subprocess.run(['node',str(PORT/'tools/disk.mjs'),str(tmp),str(image)],check=True)
    # Avoid the normal guest's .250 address if it is already running.
    commands=('ifconfig eth0 192.168.64.249\ndig example.com A\necho DIG_LIVE:$?\nhost example.com\necho HOST_LIVE:$?\n'
              'htget http://example.com/ /example.html\necho HTTP_LIVE:$?\n'
              'wc -c /example.html\ngrep "Example Domain" /example.html\necho BODY_LIVE:$?\nshutdown\n')
    command=host_command(argparse.Namespace(build=build,backend='rvm',
        vm=str(ROOT/'target/release/rvm'),vm_args=['--jit','--headless','--input',commands,'--timeout','180']),image)
    assert command[command.index('--net')+1]=='shared','This test checks the default shared network policy'
    result=subprocess.run(command,capture_output=True,text=True,timeout=190)
    text=result.stdout.replace('\r','')+result.stderr
    Path('/tmp/fuzix-nettools-live.log').write_text(text)
    assert result.returncode==0,text[-12000:]
    for token in ['DIG_LIVE:0\n','HOST_LIVE:0\n','HTTP_LIVE:0\n','BODY_LIVE:0\n','disk synced; power off.']:
        assert token in text,(token,text[-12000:])
    subprocess.run(['python3',str(PORT/'tools/fsck.py'),str(image)],check=True)
print('Default shared networking: public DNS, hostname lookup and Internet HTTP download passed')
