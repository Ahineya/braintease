#!/usr/bin/env python3
"""Run the text/pager utility batch and larger filesystem through real guests."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
PORT = ROOT / 'ports/fuzix'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, default=ROOT / 'build/fuzix')
p.add_argument('--engine', choices=('both', 'jit', 'interpreter'), default='both')
a = p.parse_args()
build = a.build.resolve()

SCRIPT = r'''mkdir /text
cd /text
join /left /right > join
join -a1 /left /right > join-all
join -o 1.2 2.2 /left /right > join-fields
join -t: -1 2 /colon-left /colon-right > join-colon
join -j 0 /left /right; echo JOIN_BAD:$?
join /long /right; echo JOIN_LONG:$?
split -l 2 /lines pieces
cat piecesaa piecesab piecesac > joined
split -0 /lines bad; echo SPLIT_BAD:$?
split -1 piecesaa pieces; echo SPLIT_SAME:$?
column -t -s: /columns > columns
column -c 1 /no-eol > narrow
column -c 65537 /lines; echo COLUMN_BAD:$?
fmt -w 12 /paragraph > formatted
fmt -w 0 /paragraph; echo FMT_BAD:$?
pr -t /lines > printed
pr -t -2 -l4 -w20 /lines > printed-cols
pr -0 /lines; echo PR_BAD:$?
col -bx < /overstrike > plain
col -x < /long; echo COL_LONG:$?
tsort /graph > order
tsort /cycle > cycle; echo TSORT_CYCLE:$?
echo odd | tsort; echo TSORT_ODD:$?
more /lines > more
less /lines > less
less /missing > missing; echo LESS_MISSING:$?
clear > clear
stty -a > stty
stty -g > tty-settings
stty made-up; echo STTY_BAD:$?
tty > tty
echo input | tty -s; echo TTY_PIPE:$?
groups > groups
logname > logname
nice -n 3 echo nice_arguments > nice
nice -n3 echo attached_increment >> nice
nice missing_program; echo NICE_MISSING:$?
time sh -c 'exit 7'; echo TIME_EXIT:$?
nohup echo nohup_redirected > nohup
nohup echo nohup_terminal
nohup missing_program > no-command; echo NOHUP_MISSING:$?
file=`mktemp /tmp/file.XXXXXXXXXX`
test -f "$file"; echo MKTEMP_FILE:$?
ls -l "$file" > file-mode
rm "$file"
folder=`mktemp -d /tmp/dir.XXXXXX`
test -d "$folder"; echo MKTEMP_DIR:$?
find "$folder" -perm 700 -type d > dir-mode
rmdir "$folder"
mktemp /tmp/bad; echo MKTEMP_BAD:$?
cksum /ascii > checksum
cksum /empty > empty-checksum
cksum /native > native-checksum; echo CKSUM_NATIVE:$?
cp /native-pattern native-copy
cmp /native-pattern native-copy; echo NATIVE_COPY:$?
dd if=/native of=/sparse bs=1 seek=1067006
dd if=/sparse of=/sparse-tail bs=1 skip=1067006
cmp /native /sparse-tail; echo SPARSE_COPY:$?
cp /empty /sparse
sh /exec-limit; echo EXEC_LIMIT:$?
sync
shutdown
'''


def run(image, jit):
    log = Path('/tmp/fuzix-texttools-' + ('jit' if jit else 'interpreter') + '.log')
    command = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'), '--disk', str(image),
               '--memory', '5120000', '--headless', '--input', 'sh /utility-test\n', '--timeout', '300']
    if jit:
        command.append('--jit')
    with log.open('w') as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=310)
    output = log.read_text().replace('\r', '')
    assert result.returncode == 0 and 'disk synced; power off.' in output, output
    for token in ('JOIN_BAD:1', 'JOIN_LONG:1', 'SPLIT_BAD:1', 'SPLIT_SAME:1', 'COLUMN_BAD:1',
                  'EXEC_LIMIT:1', 'FMT_BAD:64', 'PR_BAD:1', 'COL_LONG:2', 'TSORT_CYCLE:1', 'TSORT_ODD:1',
                  'LESS_MISSING:1', 'STTY_BAD:1', 'TTY_PIPE:1', 'NICE_MISSING:127',
                  'TIME_EXIT:7', 'NOHUP_MISSING:127', 'MKTEMP_FILE:0', 'MKTEMP_DIR:0',
                  'MKTEMP_BAD:1', 'CKSUM_NATIVE:1', 'NATIVE_COPY:0', 'SPARSE_COPY:0'):
        assert token in output, (token, output)
    subprocess.run(['python3', str(PORT / 'tools/fsck.py'), str(image)], check=True)


def read_files(image):
    # Independent decoder for the V2 inode and indirect-pointer layout.
    data = subprocess.check_output(['node', '--input-type=module', '-e', """
import fs from 'node:fs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
process.stdout.write(fatFile(fs.readFileSync(process.argv[1]), 'ROOTFS.IMG').data);
""", str(image)], cwd=ROOT)
    def w(p):
        return int.from_bytes(data[2*p:2*p+2], 'little')
    def node(ino):
        return (2 + ino // 32) * 1024 + (ino % 32) * 32
    def cells(ino):
        base = node(ino)
        size = w(base + 4) + 65536 * w(base + 5)
        def block(n):
            if n < 18:
                return w(base + 12 + n)
            n -= 18
            if n < 1024:
                return w(w(base + 30) * 1024 + n)
            n -= 1024
            return w(w(w(base + 31) * 1024 + n // 1024) * 1024 + n % 1024)
        return [w(block(i // 1024) * 1024 + i % 1024) if block(i // 1024) else 0 for i in range(size)]
    def read(path):
        ino = 1
        for part in path.strip('/').split('/'):
            directory = cells(ino)
            for i in range(0, len(directory), 32):
                name = ''.join(chr(v) for v in directory[i+1:i+31]).split('\0')[0]
                if directory[i] and name == part:
                    ino = directory[i]
                    break
            else:
                raise AssertionError('missing ' + path)
        return cells(ino)
    return read


with tempfile.TemporaryDirectory(prefix='fuzix-texttools-') as temporary:
    work = Path(temporary)
    ascii_text = b'abc\n123456789\n'
    source = work / 'ascii'
    source.write_bytes(ascii_text)
    crc = subprocess.check_output(['/usr/bin/cksum', str(source)]).decode().split()[:2]
    files = {'left': 'a first\na second\nb third\nc alone\n',
             'right': 'a red\na blue\nb green\n',
             'colon-left': 'first:a\nsecond:b\n', 'colon-right': 'a:red\nb:green\n',
             'columns': 'name:age\nAlice:7\nBob:12\n', 'no-eol': 'unterminated',
             'lines': 'one\ntwo\nthree\nfour\nfive\n',
             'paragraph': 'one two three four five six seven eight\n',
             'overstrike': 'A\bA_\bB\n', 'long': 'x'*1100 + '\n',
             'graph': 'a b\nb c\na d\n', 'cycle': 'a b\nb a\n',
             'ascii': ascii_text.decode(), 'empty': '', 'utility-test': SCRIPT,
             'exec-limit': '/bin/true' + ' ab' * 90 + '\n'}
    seed = [{'name': '/' + name, 'mode': 0o100644, 'data': list(map(ord, text))}
            for name, text in files.items()]
    native = [0, 0x1234, 0xffff, 0xabcd, 10]
    pattern = [(i * 31337 + 19) & 65535 for i in range(80000)]
    seed += [{'name': '/native', 'data': native}, {'name': '/native-pattern', 'data': pattern}]
    manifest = work / 'seed.json'
    manifest.write_text(json.dumps(seed))
    for jit in ((True, False) if a.engine == 'both' else (a.engine == 'jit',)):
        image = work / ('jit.img' if jit else 'interpreter.img')
        subprocess.run(['node', str(PORT / 'tools/disk.mjs'), str(build), str(image)], check=True)
        subprocess.run(['node', '--input-type=module', '-e', """
import fs from 'node:fs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
const disk = fs.readFileSync(process.argv[1]), root = fatFile(disk, 'ROOTFS.IMG');
fs.writeFileSync(process.argv[1], root.replace(installRoot(root.data, JSON.parse(fs.readFileSync(process.argv[2])))));
""", str(image), str(manifest)], cwd=ROOT, check=True)
        run(image, jit)
        read = read_files(image)
        text = lambda path: ''.join(map(chr, read('/text/' + path)))
        expected = 'a first red\na first blue\na second red\na second blue\nb third green\n'
        assert text('join') == expected, text('join')
        assert text('join-all') == expected + 'c alone\n'
        assert text('join-fields') == 'first red\nfirst blue\nsecond red\nsecond blue\nthird green\n'
        assert text('join-colon') == 'a:first:red\nb:second:green\n'
        assert text('joined') == files['lines']
        assert text('columns') == 'name   age\nAlice  7\nBob    12\n', repr(text('columns'))
        assert text('narrow') == 'unterminated\n'
        assert text('formatted').split() == files['paragraph'].split()
        assert all(len(line) <= 12 for line in text('formatted').splitlines())
        assert text('printed') == files['lines'], repr(text('printed'))
        assert text('printed-cols').split() == ['one', 'five', 'two', 'three', 'four'], repr(text('printed-cols'))
        assert text('plain') == 'AB\n', repr(text('plain'))
        order = text('order').split()
        assert order.index('a') < order.index('b') < order.index('c') and order.index('a') < order.index('d')
        assert text('more') == files['lines'] and text('less') == files['lines']
        assert text('clear') == '\x1b[H\x1b[2J'
        assert 'speed' in text('stty') and 'icanon' in text('stty')
        assert text('tty') == '/dev/tty1\n'
        assert text('groups') == 'root\n' and text('logname') == 'root\n'
        assert text('nice') == 'nice_arguments\nattached_increment\n'
        assert text('nohup') == 'nohup_redirected\n' and text('nohup.out') == 'nohup_terminal\n'
        assert '-rw-------' in text('file-mode'), text('file-mode')
        assert text('dir-mode').startswith('/tmp/dir.'), text('dir-mode')
        assert text('checksum').split()[:2] == crc
        assert text('empty-checksum').split()[:2] == ['4294967295', '0']
        assert read('/text/native-copy') == pattern
        assert read('/sparse-tail') == native and read('/sparse') == []
        print(('JIT' if jit else 'Interpreter') + ': text utilities, redirected pagers and V2 disk I/O passed', flush=True)
