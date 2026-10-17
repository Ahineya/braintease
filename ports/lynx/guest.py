"""Disposable actual guests for the Lynx acceptance tests."""
import fcntl
import json
import os
import pty
import select
import shutil
import signal
import struct
import subprocess
import termios
import time
import tty

from probe import PORT, ROOT


def native_image(image, kernel, names=('sh', 'echo'), blocks=8192):
    builder = (ROOT / 'ports/fuzix/tools/disk.mjs').as_uri()
    apps = [{'name': '/bin/' + name, 'path': str(kernel.parent / 'apps' / (name + '.fxe'))}
            for name in names]
    source = ('import fs from "node:fs";import {rootImage,atomicWrite} from ' + repr(builder) + ';'
              'atomicWrite(process.argv[1],rootImage(['
              '{name:"/init",mode:0o100755,data:Buffer.from("FXN1:init:1\\n")},'
              '...JSON.parse(process.argv[2]).map(a=>({name:a.name,mode:0o100755,data:fs.readFileSync(a.path)})),'
              '{name:"/dev/tty",mode:0o20666,device:0x200},'
              '{name:"/dev/proc",mode:0o20666,device:0x103},'
              '...Array.from({length:4},(_,i)=>({name:"/dev/ptyp"+i,mode:0o20666,device:0x0300+i})),'
              '...Array.from({length:4},(_,i)=>({name:"/dev/ttyp"+i,mode:0o20666,device:0x202+i})),'
              '{name:"/etc/passwd",data:Buffer.from("root:x:0:0:root:/:/bin/sh\\n")},'
              '{name:"/etc/resolv.conf",data:Buffer.from("nameserver 192.168.64.1\\n")},'
              '{name:"/etc/termcap",data:Buffer.from(process.argv[3])}'
              '],Number(process.argv[4]),128));')
    subprocess.run(['node', '--input-type=module', '-e', source, str(image), json.dumps(apps),
                    'ansi|vt100|xterm|xterm-256color:co#80:li#24:cl=\\E[H\\E[2J:ho=\\E[H:cm=\\E[%i%d;%dH:me=\\E[0m:md=\\E[1m:us=\\E[4m:ue=\\E[0m:ce=\\E[K:up=\\E[A:bc=^H:\n', str(blocks)], check=True)


def ripple_image(directory, image, build, executable, names=('init', 'sh', 'echo')):
    names = [*names, 'lynx']
    for name in names:
        (directory / (name + '.rxe')).symlink_to(executable if name == 'lynx' else build / (name + '.rxe'))
    (directory / 'FUZIX.SYS').symlink_to(build / 'FUZIX.SYS')
    (directory / 'apps.json').write_text(json.dumps(names))
    tools = ROOT / 'ports/fuzix/tools'
    subprocess.run(['node', str(tools / 'disk.mjs'), str(directory), str(image)], check=True)
    source = '''
import fs from 'node:fs';
import {fatFile} from './ports/fuzix/tools/fat.mjs';
import {openImage} from './ports/fuzix/tools/image.mjs';
import {atomicWrite} from './ports/fuzix/tools/disk.mjs';
import {installRoot} from './ports/fuzix/tools/install.mjs';
const image = process.argv[1], fixtures = process.argv[2];
const root = fatFile(openImage(image), 'ROOTFS.IMG');
const files = fs.readdirSync(fixtures).map(name => ({name:'/www/'+name,mode:0o100644,
  data:fs.readFileSync(fixtures+'/'+name)}));
files.push({name:'/etc/resolv.conf',data:Buffer.from('nameserver 192.168.64.1\\n')});
atomicWrite(image, root.replace(installRoot(root.data, files)));
'''
    subprocess.run(['node', '--input-type=module', '-e', source, str(image), str(directory / 'share')],
                   cwd=ROOT, check=True)


class Guest:
    def __init__(self, args, directory, peer=None, network=None):
        self.native = args.target == 'native'
        executable = (args.executable or PORT / ('build/lynx.fxe' if self.native else 'build/lynx.rxe')).resolve()
        share = directory / 'share'
        shutil.copytree(PORT / 'fixtures', share)
        image = directory / 'root.img'
        names = ('sh', 'echo', 'cat', 'stty', 'sleep', 'kill', 'telnetd', 'ps', 'ifconfig', 'host')
        if self.native:
            shutil.copy2(executable, share / 'lynx')
            native_image(image, args.kernel.resolve(), names)
            command = [str(args.kernel.resolve()), '--run', str(image), '--share', str(share)]
            self.program, self.pages, self.prompt = '/mnt/host/lynx', '/mnt/host', b'fuse-x# '
        else:
            config = share / 'lynx.cfg'
            config.write_text(config.read_text().replace('/mnt/host/', '/www/'))
            build = args.build_dir.resolve()
            ripple_image(directory, image, build, executable, ('init', *names))
            command = [str(ROOT / 'target/release/rvm'), str(build / 'bios.bin'),
                       '--disk', str(image), '--memory', '16384000', '--net', network or str(directory / 'nic'),
                       '--jit', '--timeout', '900']
            self.program, self.pages, self.prompt = '/bin/lynx', '/www', b'fuzix# '
        self.peer = peer
        self.log = bytearray()
        self.at = 0
        self.master, slave = pty.openpty()
        tty.setraw(slave)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
        self.process = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave,
            start_new_session=True, env=dict(os.environ, RCC_NATIVE_HEADLESS='1',
            RCC_NATIVE_NET=network or str(directory / 'nic'), RCC_NATIVE_TIMEOUT='900',
            http_proxy='', https_proxy='', HTTP_PROXY='', HTTPS_PROXY='', ALL_PROXY='', no_proxy='*'))
        os.close(slave)
        self.screen = Screen()
        self.logfile = PORT / f'build/browser-{args.target}.log'

    def pump(self):
        if self.peer:
            self.peer.pump()
        if select.select([self.master], [], [], .01)[0]:
            try:
                data = os.read(self.master, 65536)
            except OSError:
                data = b''
            self.log.extend(data)
            self.screen.feed(data)
            self.logfile.write_bytes(self.log)

    def wait(self, condition, description, seconds=180):
        deadline = time.monotonic() + seconds
        while not condition():
            self.pump()
            if self.process.poll() is not None or time.monotonic() > deadline:
                raise AssertionError(description + '\nSCREEN:\n' + self.screen.text() + '\nLOG:\n'
                                     + self.log[-6000:].decode(errors='replace'))

    def raw(self, marker, seconds=180):
        self.wait(lambda: marker in bytes(self.log[self.at:]).replace(b'\r', b''), repr(marker), seconds)
        # Keep offsets in raw bytes; match line endings without consuming future output.
        normalized = bytes(self.log[self.at:]).replace(b'\r', b'')
        end = normalized.index(marker) + len(marker)
        count = 0
        raw_end = self.at
        while count < end:
            if self.log[raw_end] != 13:
                count += 1
            raw_end += 1
        result = bytes(self.log[self.at:raw_end]).replace(b'\r', b'')
        self.at = raw_end
        return result

    def send(self, data):
        os.write(self.master, data)

    def shell(self, command):
        self.send(command.encode() + b'\r')
        return self.raw(self.prompt)

    def visible(self, text, seconds=180):
        self.wait(lambda: text in self.screen.text(), text, seconds)

    def goto(self, url):
        # Lynx deliberately flushes typeahead when dismissing some alerts.
        # Wait for its actual input prompt before pasting the next address;
        # an early G may need repeating while an alert is still being cleared.
        next_key = 0
        def ready():
            nonlocal next_key
            if 'URL to open:' in self.screen.text():
                return True
            now = time.monotonic()
            if now >= next_key:
                self.send(b'g')
                next_key = now + 1
            return False
        self.wait(ready, 'URL entry prompt')
        start = len(self.log)
        self.send(b'\x15' + url.encode() + b'\r')
        return start

    def resize(self, rows, columns):
        self.screen.resize(rows, columns)
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack('HHHH', rows, columns, 0, 0))
        os.kill(self.process.pid, signal.SIGWINCH)

    def close(self):
        self.logfile.write_bytes(self.log)
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        os.close(self.master)


class Screen:
    """ANSI text sink so UI assertions inspect the current page, not old output."""
    def __init__(self):
        self.rows, self.columns = 24, 80
        self.lines = [list(' ' * self.columns) for _ in range(self.rows)]
        self.y = self.x = 0
        self.pending = ''
        self.wrap = False

    def feed(self, data):
        self.pending += data.decode('latin1')
        text, i = self.pending, 0
        while i < len(text):
            ch = text[i]
            if ch == '\x1b':
                if i + 1 == len(text): break
                if text[i + 1] == '[':
                    end = i + 2
                    while end < len(text) and not ('@' <= text[end] <= '~'): end += 1
                    if end == len(text): break
                    parameters, final = text[i + 2:end], text[end]
                    if not parameters.startswith('?'):
                        numbers = [int(n) if n else 0 for n in parameters.split(';')]
                        n = numbers[0]
                        if final in 'Hf': self.y, self.x = max(0, n - 1), max(0, (numbers[1] if len(numbers)>1 else 1) - 1)
                        elif final == 'A': self.y = max(0, self.y - (n or 1))
                        elif final == 'B': self.y = min(self.rows - 1, self.y + (n or 1))
                        elif final == 'C': self.x = min(self.columns - 1, self.x + (n or 1))
                        elif final == 'D': self.x = max(0, self.x - (n or 1))
                        elif final == 'J' and n == 2: self.lines = [list(' ' * self.columns) for _ in range(self.rows)]
                        elif final == 'K' and self.y < self.rows:
                            for column in range(self.x if n == 0 else 0, self.columns if n != 1 else self.x + 1): self.lines[self.y][column] = ' '
                        if final in 'HfABCDJK': self.wrap = False
                    i = end + 1
                    continue
                if text[i + 1] in '()':
                    if i + 2 == len(text): break
                    i += 3
                else: i += 2
                continue
            if ch == '\r': self.x, self.wrap = 0, False
            elif ch == '\n': self.y += 1; self.wrap = False
            elif ch == '\b': self.x = max(0, self.x - 1); self.wrap = False
            elif ch >= ' ':
                if self.wrap: self.x = 0; self.y += 1; self.wrap = False
                if self.y >= self.rows: self.lines.pop(0); self.lines.append(list(' ' * self.columns)); self.y = self.rows - 1
                if self.x < self.columns: self.lines[self.y][self.x] = ch
                if self.x == self.columns - 1: self.wrap = True
                else: self.x += 1
            if self.y >= self.rows: self.lines.pop(0); self.lines.append(list(' ' * self.columns)); self.y = self.rows - 1
            i += 1
        self.pending = text[i:]

    def text(self):
        return '\n'.join(''.join(line) for line in self.lines)

    def resize(self, rows, columns):
        self.lines = [(self.lines[y][:columns] if y < self.rows else []) + [' '] * max(0, columns - (min(self.columns, columns) if y < self.rows else 0)) for y in range(rows)]
        self.rows, self.columns = rows, columns
        self.y, self.x = min(self.y, rows - 1), min(self.x, columns - 1)
        self.wrap = False
