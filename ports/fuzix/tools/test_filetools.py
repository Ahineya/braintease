#!/usr/bin/env python3
"""Exercise find/dd/diff/tar on disk, across reboot, and against host USTAR."""
import argparse
from root_reader import RootReader
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile

PORT=Path(__file__).resolve().parents[1]
ROOT=PORT.parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,default=ROOT/'build/fuzix')
p.add_argument('--engine', choices=('both', 'jit', 'interpreter'), default='jit', help='RVM engine coverage (default: jit)')
options = p.parse_args()
engines = [True, False] if options.engine == 'both' else [options.engine == 'jit']
engine_label = 'JIT and interpreter' if options.engine == 'both' else options.engine
build = options.build.resolve()

def host_tar(entries):
    stream=io.BytesIO()
    with tarfile.open(fileobj=stream,mode='w',format=tarfile.USTAR_FORMAT) as archive:
        for name,data in entries:
            info=tarfile.TarInfo(name)
            info.size=len(data);info.mode=0o640;info.mtime=2000000000
            archive.addfile(info,io.BytesIO(data))
    return list(stream.getvalue())

def seed(image,files):
    source="""
import fs from 'node:fs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
const image=openImage(process.argv[1]),root=fatFile(image,'ROOTFS.IMG');
atomicWrite(process.argv[1],root.replace(installRoot(root.data,JSON.parse(fs.readFileSync(process.argv[2],'utf8')))));
"""
    manifest=image.with_suffix('.json');manifest.write_text(json.dumps(files))
    subprocess.run(['node','--input-type=module','-e',source,str(image),str(manifest)],cwd=ROOT,check=True)

def run(image,commands,jit):
    log=image.with_suffix('.'+('jit' if jit else 'interpreter')+'.log')
    command=[str(ROOT/'target/release/rvm'),str(build/'bios.bin'),'--disk',str(image),'--memory','16384000','--headless','--input',commands,'--timeout','360']
    if jit:command+=['--jit']
    with log.open('w') as stream:
        result=subprocess.run(command,stdout=stream,stderr=subprocess.STDOUT,text=True,timeout=370)
    text=log.read_text().replace('\r','').split('Starting /init\n')[-1]
    text='\n'.join(line for line in text.splitlines()
                   if not line.startswith(('riplix# ', '> ')))
    Path('/tmp/fuzix-filetools-output.log').write_text(text)
    assert result.returncode==0 and 'disk synced; power off.' in text,text
    assert 'panic:' not in text and 'FAILED' not in text and 'corrupt superblock' not in text,text
    return text

# Read native cells from a clean filesystem, without mounting or modifying it.
def read_root(image):
    reader = RootReader(image)
    return reader.read, lambda path: reader.base(reader.lookup(path)), reader.word

FIRST=r"""mkdir -p /work/tree/nested /work/out
cd /work/tree
echo alpha > first
 echo -e 'alpha\012changed\012last' > left
 echo -e 'alpha\012new\012last' > right
cp /native native
 echo spaced > 'space name'
find . -name '*name' -exec cp {} /work/copied \;
cat /work/copied
find . -type f -name f* -print
find first -type f
find . \( -name first -o -name right \) -type f
find /missing; echo FIND_ERROR:$?
find . -unknown; echo FIND_BAD:$?
find . -name first -size 6c
find . -name first -size 1
find . -name native -perm 640 -print > /perm-matches
find . -name; echo FIND_MISSING:$?
find . -perm 644 -type f | wc -l
dd if=first of=copy bs=2 count=2
cat copy
 dd if=first of=empty count=0
wc -c empty
 echo -n abcdefgh > seeked
 dd if=first of=seeked bs=2 count=1 seek=2
cat seeked
 dd if=first bs=1 count=1 | wc -c
 dd if=first of=bad bs=0; echo DD_BAD:$?
 dd if=first of=bad seek=2147483647 bs=512; echo DD_OVERFLOW:$?
 dd if=first of=first; echo DD_SAME:$?
cat first
dd if=/dev/zero of=/large bs=1000 count=70
wc -c /large
find /large -size 70000c -print > /size-matches
tar -cf /large.tar /large
 diff left right; echo DIFF_CHANGED:$?
 diff first first; echo DIFF_SAME:$?
 diff /missing right; echo DIFF_ERROR:$?
 echo alpha | diff - first; echo DIFF_STDIN:$?
 diff -h left right; echo DIFF_SMALL:$?
mkdir compare-dir
cp first compare-dir/first
diff first compare-dir; echo DIFF_DIR:$?
diff -h first compare-dir; echo DIFFH_DIR:$?
 echo -n end > no-newline
 echo -n end2 > no-newline2
 diff no-newline no-newline2; echo DIFF_EOF:$?
 echo -n end > same-no-newline
 diff no-newline same-no-newline; echo DIFF_EOF_SAME:$?
 echo -e 'a  b' > blanks
 echo -e 'a b' > blanks2
 diff -b blanks blanks2; echo DIFF_BLANKS:$?
diff /many-left /many-right > /many.patch; echo DIFF_MANY:$?
cd /work
 tar -cf /bundle.tar tree; echo TAR_CREATE:$?
 tar -tf /bundle.tar
cd out
 tar -xf /bundle.tar; echo TAR_EXTRACT:$?
 tar -xf /large.tar; echo TAR_LARGE:$?
 cmp tree/native /native
 cmp tree/first /work/tree/first
 cmp 'tree/space name' '/work/tree/space name'
 tar -xkf /bundle.tar; echo TAR_KEEP:$?
 tar -cf /pipe.tar tree
 cat /pipe.tar | tar -tf -
 tar -cb 4 -f /blocked.tar tree
 tar -tb 4 -f /blocked.tar
mkdir pipeout
cd pipeout
 tar -cf - /work/tree | tar -xf -; echo TAR_PIPE:$?
 cmp work/tree/native /native
cd /work/out
 tar -xf /host.tar; echo TAR_HOST:$?
cat from-host
find from-host -mtime -0 > /future-matches
find from-host -newer /native > /newer-matches
 tar -xf /unsafe.tar; echo TAR_UNSAFE:$?
 test -f /escaped; echo ESCAPED:$?
 tar -tf /bad.tar; echo TAR_BAD:$?
 tar -tf /short.tar; echo TAR_SHORT:$?
 tar -cb 100 -f /bad-factor.tar tree; echo TAR_FACTOR:$?
sync
shutdown
"""

with tempfile.TemporaryDirectory(prefix='fuzix-filetools-') as temporary:
    image=Path(temporary)/'tools.img'
    subprocess.run(['node',str(PORT/'tools/disk.mjs'),str(build),str(image)],check=True)
    bad=host_tar([('bad',b'bad')]);bad[0]^=1
    many_left=''.join(f'line {i:04d}\n' for i in range(500))
    many_right=many_left.replace('line 0250\n','replacement\n').replace('line 0370\n','')+'appended\n'
    a=Path(temporary)/'left';b=Path(temporary)/'right'
    a.write_text(many_left);b.write_text(many_right)
    expected_diff=subprocess.run(['/usr/bin/diff',str(a),str(b)],stdout=subprocess.PIPE,check=False).stdout
    seed(image,[
        {'name':'/many-left','mode':0o100644,'data':list(many_left.encode())},
        {'name':'/many-right','mode':0o100644,'data':list(many_right.encode())},
        {'name':'/native','mode':0o100640,'data':[0,0x1234,0xffff,0x80,10]},
        {'name':'/host.tar','mode':0o100644,'data':host_tar([('from-host',b'host payload\n')])},
        {'name':'/unsafe.tar','mode':0o100644,'data':host_tar([('../../escaped',b'NO')])},
        {'name':'/bad.tar','mode':0o100644,'data':bad},
        {'name':'/short.tar','mode':0o100644,'data':[65]*200},
    ])
    deep='/deep/'+'/'.join(['directory']*14)
    deep_commands=(f'mkdir -p {deep}\necho deep_payload > {deep}/needle\n'
                   'find /deep -name needle; echo FIND_DEEP:$?\n'
                   'tar -cf /deep.tar /deep\nmkdir /deep-out\ncd /deep-out\n'
                   'tar -xf /deep.tar; echo TAR_DEEP:$?\ncd /\n')
    output=run(image,deep_commands+FIRST,engines[0])
    for expected in ['FIND_DEEP:0','TAR_DEEP:0','spaced\n','./first','FIND_ERROR:1','FIND_BAD:1','FIND_MISSING:1','DD_BAD:1','DD_OVERFLOW:1',
                     'DD_SAME:1','DIFF_CHANGED:1','DIFF_SAME:0','DIFF_ERROR:2','DIFF_STDIN:0',
                     'DIFF_SMALL:1','DIFF_DIR:0','DIFFH_DIR:0','DIFF_EOF:1','DIFF_EOF_SAME:0','DIFF_BLANKS:0','DIFF_MANY:1',
                     'No newline at end of file','TAR_CREATE:0','TAR_EXTRACT:0','TAR_LARGE:0','TAR_KEEP:1',
                     'TAR_PIPE:0','TAR_HOST:0','host payload','TAR_UNSAFE:1','ESCAPED:1',
                     'TAR_BAD:1','TAR_SHORT:1','TAR_FACTOR:1']:
        assert expected in output,(expected,output)
    subprocess.run(['python3',str(PORT/'tools/fsck.py'),str(image)],check=True)
    get,base,w=read_root(image)
    assert get('/work/tree/copy')==list(b'alph')
    assert get('/work/tree/empty')==[]
    assert get('/work/tree/seeked')==list(b'abcdal')
    assert get('/work/tree/first')==list(b'alpha\n')
    assert get('/large')==[0]*70000
    assert get('/work/out/large')==[0]*70000
    assert get('/deep-out'+deep+'/needle')==list(b'deep_payload\n')
    assert bytes(get('/many.patch'))==expected_diff
    assert get('/perm-matches')==list(b'./native\n')
    assert get('/size-matches')==list(b'/large\n')
    assert get('/future-matches')==list(b'from-host\n')
    assert get('/newer-matches')==list(b'from-host\n')
    mtime=lambda path:w(base(path)+8)+65536*w(base(path)+9)
    assert mtime('/work/out/from-host')==2000000000
    assert mtime('/work/out/tree')==mtime('/work/tree')
    for path in ['/work/out/tree/native','/work/out/pipeout/work/tree/native']:
        assert get(path)==[0,0x1234,0xffff,0x80,10],path
        assert w(base(path))&0o777==0o640,path
    archive=get('/bundle.tar')
    # All header bytes have conventional USTAR values; native data is preserved
    # as cells. Inspect the ASCII-only archive independently with Python tarfile.
    small=get('/pipe.tar')
    octets=bytes(c&255 for c in small)
    with tarfile.open(fileobj=io.BytesIO(octets),mode='r:') as tar:
        assert tar.extractfile('tree/first').read()==b'alpha\n'
        assert tar.getmember('tree/native').mode==0o640
    output=run(image,'cd /work/out\nfind tree -name native -exec cmp {} /native \\;\ndd if=tree/native of=/reboot-native bs=2\ndiff tree/left tree/right; echo REBOOT_DIFF:$?\ntar -cf /reboot.tar tree\nmkdir reboot\ncd reboot\ntar -xf /reboot.tar\ncmp tree/native /native\necho REBOOT_OK\nshutdown\n',engines[-1])
    assert 'REBOOT_DIFF:1' in output and 'REBOOT_OK' in output,output
    get,base,w=read_root(image)
    assert get('/reboot-native')==[0,0x1234,0xffff,0x80,10]
    subprocess.run(['python3',str(PORT/'tools/fsck.py'),str(image)],check=True)
print('File tools passed: find expressions/exec, dd count/seek, diff statuses/EOF/stdin, USTAR/streams/native data/metadata, malformed input and reboot persistence; ' + engine_label)
