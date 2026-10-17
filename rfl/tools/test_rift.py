#!/usr/bin/env python3
"""Drive Rift on both real kernels using disposable disks and a real terminal."""
import argparse
import json
from pathlib import Path
import tempfile
from test_os import Guest, ROOT, PORT, run

F1, F2, F3, F4, F5 = b'\x1bOP', b'\x1bOQ', b'\x1bOR', b'\x1bOS', b'\x1b[15~'
F6, F7, F8, F9, F10 = [f'\x1b[{n}~'.encode() for n in (17,18,19,20,21)]

def make_image(system, build, artifact, directory):
    files = []
    for name in ['sh','cat','echo','true','bf','tclsh','awk','make','m4','dc','chmod','sleep','mkdir','cp']:
        source = build / (name + '.rxe') if system == 'fuzix' else build / 'apps' / (name + '.fxe')
        assert source.is_file(), source
        files.append({'name':'/bin/'+name, 'source':str(source)})
    files.append({'name':'/bin/rift','source':str(artifact)})
    if system == 'fuzix': files.append({'name':'/init','source':str(build / 'init.rxe')})
    fixtures = {
        '/etc/termcap':r'ansi|vt100|xterm|xterm-256color|fuse-x:co#80:li#24:cl=\E[H\E[2J:ho=\E[H:cm=\E[%i%d;%dH:me=\E[0m:md=\E[1m:us=\E[4m:ue=\E[0m:ce=\E[K:up=\E[A:bc=^H:'+'\n',
        '/tmp/main.rfl':'fn main() {\n    call(one, nested(two));\n}\n',
        '/tmp/other.txt':'other tab\n',
        '/tmp/a\'b $x.bf':'++++++++[>++++++++<-]>+.\n',
        '/tmp/test.tcl':'puts RIFT_TCL_OK\n',
        '/tmp/test.awk':'BEGIN { print "RIFT_AWK_OK" }\n',
        '/tmp/modify.sh':'echo EXTERNAL > /tmp/main.rfl\n',
        '/tmp/interrupt.sh':'echo RIFT_CHILD_READY; sleep 10\n',
        '/tmp/crlf.txt':'one\r\ntwo\r\n',
        '/tmp/unsafe.bin':'bad\0binary',
        '/tmp/.rift-config':'[editor]\ntabstop=2\n[run]\nbrainfuck=echo RIFT_CUSTOM_OK {file}\n',
        '/tmp/big.txt':('0123456789abcdef' * 4375) + '\n',
    }
    for name, data in fixtures.items():
        source = directory / ('fixture-' + str(len(files)))
        source.write_bytes(data.encode())
        files.append({'name':name,'source':str(source),'mode':0o100644})
    manifest = directory / 'files.json'; manifest.write_text(json.dumps(files))
    image = directory / 'disk.img'
    script = f'''
import fs from 'node:fs';
import {{rootImage}} from '{(PORT / 'tools/disk.mjs').as_uri()}';
import {{writeImage}} from '{(PORT / 'tools/image.mjs').as_uri()}';
import {{riftFiles}} from '{(ROOT / 'rfl/os/files.mjs').as_uri()}';
import {{fat16Image}} from '{(ROOT / 'c-test/examples/rOS/mkfat16.mjs').as_uri()}';
import {{DISK_GEOMETRY}} from '{(PORT / 'tools/layout.mjs').as_uri()}';
const files=JSON.parse(fs.readFileSync(process.argv[1])).map(f=>({{name:f.name,mode:f.mode??0o100755,data:fs.readFileSync(f.source)}}));
files.push(...riftFiles());
for(const [name,device] of [['tty',0x200],['null',0x100],['zero',0x102]]) files.push({{name:'/dev/'+name,mode:0o20666,device,data:Buffer.alloc(0)}});
if(process.argv[2]==='fuse-x') files.push({{name:'/init',mode:0o100755,data:Buffer.from('FXN1:init:1\\n')}});
const root=rootImage(files,65536,1024);
if(process.argv[2]==='fuse-x') writeImage(process.argv[3],root,true);
else {{ const {{Image}}=await import('{(PORT / 'tools/image.mjs').as_uri()}');
writeImage(process.argv[3],fat16Image([{{name:'FUZIX.SYS',data:fs.readFileSync(process.argv[4]+'/FUZIX.SYS')}},{{name:'ROOTFS.IMG',data:root}}],{{...DISK_GEOMETRY,imageFactory:size=>new Image(size)}}),true); }}
'''
    run(['node','--input-type=module','-e',script,manifest,system,image,build])
    return image

def exercise(args, system, directory):
    build = (args.fuzix_build if system == 'fuzix' else args.native_build).resolve()
    artifact = (args.prebuilt / system / ('rift.' + ('rxe' if system == 'fuzix' else 'fxe'))).resolve() if args.prebuilt else directory / ('rift.'+('rxe' if system == 'fuzix' else 'fxe'))
    if not args.prebuilt: run([args.rfl.resolve(),'build',ROOT / 'rfl/apps/rift/main.rfl','--os',system,'--build-dir',build,'-o',artifact])
    if args.save_artifacts:
        import shutil
        output=args.save_artifacts.resolve()/system;output.mkdir(parents=True,exist_ok=True);shutil.copyfile(artifact,output/artifact.name)
    image=make_image(system,build,artifact,directory)
    command=[build/'fuse-x','--run',image] if system=='fuse-x' else [ROOT/'target/release/rvm',build/'bios.bin','--disk',image,'--memory','16384000','--net','off','--jit','--guest-ctrl-c','--timeout','600']
    guest=Guest(command,system)
    def command_line(command):
        guest.send(b':');guest.visible(': command');guest.send(command.encode()+b'\r')
    def stable(text):
        guest.visible(text)
        # Wait for the whole frame rather than racing its cursor/status tail.
        for _ in range(3): guest.pump()
    try:
        guest.raw(guest.prompt)
        print(system+': starting live editor checks',flush=True)
        guest.send(b'rift\r');stable('NORMAL')
        guest.visible('F10 Menu')
        assert guest.screen.text().count('F1 Help') == 1 and guest.screen.text().count('F10 Menu') == 1
        guest.send(b'i')
        for text in ['a', 'ab', 'abc']:
            guest.send(text[-1:].encode())
            guest.wait(lambda: ''.join(guest.screen.lines[2][7:10]).rstrip() == text and
                       guest.screen.y == 2 and guest.screen.x == 7 + len(text), 'successive insertion frames')
        guest.send(b'\x1b');stable('NORMAL');command_line('qa!');guest.raw(guest.prompt)
        guest.send(b'rift /tmp/main.rfl /tmp/other.txt\r');stable('NORMAL')
        assert guest.screen.alternate and 'other.txt' in guest.screen.text()
        # Actual lexical attributes: fn is gold, body background is ordinary blue.
        guest.wait(lambda: guest.screen.styles[2][7][0] == 3, 'keyword highlighting')
        assert guest.screen.styles[3][7][1] == 4
        command_line('2');stable('2:1')
        guest.send(b'f(ci)value\x1b');stable('call(value);')
        guest.send(b'u');stable('call(one, nested(two));')
        guest.send(b'\x12');stable('call(value);')
        guest.send(b'u.');stable('call(value);')
        guest.send(F2);stable('Saved /tmp/main.rfl')
        print(system+': text objects and history passed',flush=True)
        guest.send(F6);stable('/tmp/other.txt')
        guest.send(b'A changed\x1b');stable('other tab changed')
        guest.send(b'\x17');stable('Unsaved changes')
        guest.send(b'\r');stable('other tab changed') # Safe default Cancel.
        guest.wait(lambda: 'Unsaved changes' not in guest.screen.text(), 'Cancel preserves the buffer and closes the dialog')
        guest.send(F2);stable('Saved /tmp/other.txt')
        guest.send(F9);stable('/tmp/main.rfl')
        guest.send(F7);guest.visible('Find (literal)');guest.send(b'value\r');stable('Found: value')
        guest.send(b'viw');stable('VISUAL')
        assert any(style[:2]==(0,6) for row in guest.screen.styles[2:21] for style in row), 'selection color'
        guest.send(b'\x1b');stable('NORMAL')
        assert not any(style[:2]==(0,6) for row in guest.screen.styles[2:21] for style in row), 'selection cleared'
        guest.send(F1);stable("programmer's editor")
        guest.resize(6,30);stable('Rift needs')
        guest.resize(24,80);stable('NORMAL')
        guest.send(F10);stable('Rift menu');guest.send(b'\x1b');stable('NORMAL')
        guest.send(F4);stable('Tabs');guest.send(b'\x1b');stable('NORMAL')
        command_line('theme');stable('Theme (session)');guest.send(b'g\r');guest.wait(lambda: 'Theme (session)' not in guest.screen.text(), 'theme selected');stable('NORMAL')
        assert guest.screen.styles[3][7][1]==0
        command_line('theme');stable('Theme (session)');guest.send(b'm\r');guest.wait(lambda: 'Theme (session)' not in guest.screen.text(), 'mono theme selected');stable('NORMAL')
        command_line('2');guest.send(b'wviw');stable('VISUAL');assert any(s[3] for row in guest.screen.styles[2:21] for s in row);guest.send(b'\x1b')
        # Opening a NUL file must leave the current buffer intact.
        guest.send(F3);guest.visible('Open file');guest.send(b'/tmp/unsafe.bin\r');stable('Binary NUL')
        # Prompts support moving within the filename, rather than append-only editing.
        print(system+': tabs, themes and resize passed',flush=True)
        guest.send(F3);guest.visible('Open file');guest.send(b"/tmp/a'b $x.bfZ\x1b[D\x1b[3~\r");stable('brainfuck')
        guest.at=len(guest.log);guest.send(F5);guest.raw(b'Press Enter to return to Rift');assert not guest.screen.alternate
        output=bytes(guest.log[guest.at-200:guest.at]);assert b'Program wait status: 0' in output, output
        guest.send(b'\r');stable('Run finished; wait status 0')
        for path, marker in [('/tmp/test.tcl',b'RIFT_TCL_OK'),('/tmp/test.awk',b'RIFT_AWK_OK')]:
            guest.send(F3);guest.visible('Open file');guest.send(path.encode()+b'\r');stable(path)
            guest.at=len(guest.log);guest.send(F5);guest.raw(b'Rift Run:');guest.raw(marker);guest.raw(b'Press Enter to return to Rift');guest.send(b'\r');stable('Run finished; wait status 0')
        guest.send(F8);stable('Guest development tools');stable('brainfuck: /bin/bf');guest.send(b'\r');stable('NORMAL')
        guest.send(F3);guest.visible('Open file');guest.send(b'/tmp/interrupt.sh\r');stable('/tmp/interrupt.sh')
        guest.at=len(guest.log);guest.send(F5);guest.raw(b'Rift Run:');guest.raw(b'RIFT_CHILD_READY');guest.send(b'\x03');guest.raw(b'Press Enter to return to Rift',seconds=8);guest.send(b'\r');stable('Run finished; wait status')
        # Close a middle tab and continue using the surviving buffers.
        guest.send(F4);stable('Tabs');guest.send(b'\x1b[H\x1b[B\r');stable('/tmp/other.txt');guest.send(b'\x17');stable('NORMAL')
        # Saving CRLF uses the original newline convention.
        guest.send(F3);guest.visible('Open file');guest.send(b'/tmp/crlf.txt\r');stable('/tmp/crlf.txt');guest.send(b'A!\rone\x1b');stable('NORMAL');guest.send(F2);stable('Saved /tmp/crlf.txt')
        command_line('w /missing/new.rfl');stable('Save failed:')
        assert '/tmp/crlf.txt' in guest.screen.text()
        guest.send(F3);guest.visible('Open file');guest.send(b'/tmp/big.txt\r');stable('/tmp/big.txt')
        guest.send(b'ggAEND\x1b');stable('END');guest.send(F2);stable('Saved /tmp/big.txt')
        guest.send(b'\x17');stable('NORMAL')
        guest.send(F3);guest.visible('Open file');guest.send(b'/tmp/modify.sh\r');stable('/tmp/modify.sh')
        guest.at=len(guest.log);guest.send(F5);guest.raw(b'Rift Run:');guest.raw(b'Press Enter to return to Rift');guest.send(b'\r');stable('Run finished; wait status 0')
        guest.send(F4);stable('Tabs');guest.send(b'\x1b[H\r');stable('/tmp/main.rfl')
        guest.send(b'A !\x1b');stable('call(value); !');guest.send(F2);stable('File changed or exists: overwrite?')
        guest.send(b'\r');guest.wait(lambda: 'overwrite?' not in guest.screen.text(), 'conflict cancellation');stable('Save cancelled')
        guest.send(F2);stable('File changed or exists: overwrite?');guest.send(b'\x1b[A\r');stable('Saved /tmp/main.rfl')
        print(system+': runners, save conflicts and large-file editing passed',flush=True)
        command_line('qa');guest.raw(guest.prompt)
        assert not guest.screen.alternate and 'Rift  File' not in guest.screen.text()
        output=guest.shell('cat /tmp/main.rfl');assert b'call(value); !' in output and b'nested' not in output,output
        assert b'other tab changed' in guest.shell('cat /tmp/other.txt')
        guest.shell('mkdir -p /.config/rift; cp /tmp/.rift-config /.config/rift/config.ini; BF=/missing; export BF')
        guest.send(b'rift /tmp/user.txt /usr/share/rift/hello.bf\r');stable('NORMAL')
        guest.send(b'i\tX\x1b');stable('X');guest.send(F2);stable('Saved /tmp/user.txt')
        guest.send(F6);stable('/usr/share/rift/hello.bf')
        guest.at=len(guest.log);guest.send(F5);guest.raw(b'Rift Run:');guest.raw(b'RIFT_CUSTOM_OK');guest.raw(b'Press Enter to return to Rift');guest.send(b'\r');stable('Run finished; wait status 0')
        command_line('qa');guest.raw(guest.prompt)
        guest.send(b'shutdown\r');guest.process.wait(timeout=30);assert guest.process.returncode==0
        # Decode on-disk bytes independently, after shutdown, including CRLF and >64 KiB.
        import importlib.util, mmap, struct
        spec=importlib.util.spec_from_file_location('rift_root_reader',PORT/'tools/root_reader.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        if system=='fuzix': reader=module.RootReader(image)
        else:
            class RawReader(module.RootReader):
                def __init__(self,path):
                    with open(path,'rb') as file:self.disk=mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ)
                def word(self,cell):return struct.unpack_from('<H',self.disk,cell*2)[0]
            reader=RawReader(image)
        assert bytes(reader.read('/tmp/crlf.txt'))==b'one!\r\none\r\ntwo\r\n'
        assert bytes(reader.read('/tmp/big.txt'))==('0123456789abcdef'*4375+'END\n').encode()
        assert bytes(reader.read('/tmp/user.txt'))==b'  X'
        reader.disk.close()
        print(system+': live vi/text objects, undo/redo/dot, syntax/selection colors, tabs, save/cancel, menus/search, prompt editing, resize, themes/mono, binary rejection, quoted Brainfuck/Tcl/awk runs, interrupt and terminal restoration passed',flush=True)
    finally:
        Path('/tmp/rift-'+system+'.log').write_bytes(guest.log)
        if guest.process.poll() is None: guest.process.kill();guest.process.wait()
        import os
        os.close(guest.master)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os',choices=['both','fuse-x','fuzix'],default='both')
    parser.add_argument('--rfl',type=Path,default=ROOT/'target/release/rfl')
    parser.add_argument('--native-build',type=Path,default=ROOT/'fuse-x/build')
    parser.add_argument('--fuzix-build',type=Path,default=ROOT/'build/fuzix')
    parser.add_argument('--prebuilt',type=Path)
    parser.add_argument('--save-artifacts',type=Path)
    args=parser.parse_args()
    for system in (['fuse-x','fuzix'] if args.os=='both' else [args.os]):
        with tempfile.TemporaryDirectory(prefix='rift-'+system+'-',dir='/tmp') as tmp: exercise(args,system,Path(tmp))
if __name__=='__main__':main()
