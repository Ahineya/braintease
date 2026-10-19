// Disposable, deliberately small desktop integration disk; never a user's root.
import fs from 'node:fs';
import path from 'node:path';
import {rootImage} from '../../fuzix/tools/disk.mjs';
import {fat16Image} from '../../../c-test/examples/rOS/mkfat16.mjs';
import {fuseDEFiles} from '../../fusede/files.mjs';
const [build, output, profile] = process.argv.slice(2);
if (profile && profile !== '--engine') throw Error('unknown image profile');
if (!build || !output) throw Error('usage: desktop-image.mjs BUILD OUTPUT');
const names = profile === '--engine' ? ['init','sh','echo','cat','true','ifconfig','nxdraw'] : ['init', 'sh', 'echo', 'cat', 'ls', 'kill', 'sleep', 'stty', 'ps', 'true',
               'vi', 'gterm', 'nano-X', 'nanowm', 'nxlaunch', 'nxterm', 'nxcheck', 'startx', 'nxeyes', 'nxclock', 'nxroach', 'nxroachcheck', 'nxdsktop', 'nxmsg', 'nxselect', 'nxcalc', 'nxtetris', 'nxview', 'klotski'];
const files = names.map(name => ({name: name === 'init' ? '/init' : `/bin/${name}`,
    // This test drives Levee's specific insert/:wq commands. Keep its fixture
    // independent of the user's default /bin/vi (currently Rift).
    data: fs.readFileSync(path.join(build, `${name === 'vi' ? 'levee' : name}.rxe`)), mode: 0o100755}));
// Newly linked clients may use the shared SDK even when older fixture apps
// are static. Carry their runtime modules into this private image as well.
const libraryDirectory=path.join(build,'lib');
if(fs.existsSync(libraryDirectory))for(const name of fs.readdirSync(libraryDirectory).sort())
  if(/^[a-zA-Z0-9._-]+\.rsl\.[0-9]+$/.test(name))
    files.push({name:`/lib/riplix/${name}`,mode:0o100644,data:fs.readFileSync(path.join(libraryDirectory,name))});
if(profile !== '--engine')files.push(...fuseDEFiles(path.join(build,'fusede')));
files.push({name: '/dev/tty', mode: 0o20666, device: 0x200},
    {name: '/edit me;echo HACK', mode: 0o100644, data: Buffer.from('safe filename\n')},
    {name: '/dev/console', mode: 0o20600, device: 0x201},
    {name: '/dev/gfx', mode: 0o20600, device: 0x400},
    {name: '/dev/null', mode: 0o20666, device: 0x100},
    {name: '/dev/proc', mode: 0o20666, device: 0x103},
    {name: '/etc/nxlaunch.cnf', mode: 0o100644, data: Buffer.from('Terminal - /bin/nxterm\nShell - /bin/nxterm /bin/sh\nEyes - /bin/nxeyes\nClock - /bin/nxclock\nRoaches - /bin/nxroach -seed 5407\nTetris - /bin/nxtetris --seed 1853\n')});
for(let i=0;i<8;i++) files.push(
    {name: `/dev/ptyp${i}`, mode: 0o20600, device: 0x300+i},
    {name: `/dev/ttyp${i}`, mode: 0o20600, device: 0x202+i});
const root = rootImage(files, profile === '--engine' ? 32768 : 65536, 256);
fs.writeFileSync(output, fat16Image([{name: 'FUZIX.SYS', data: fs.readFileSync(path.join(build, 'FUZIX.SYS'))},
    {name: 'ROOTFS.IMG', data: root}], {totalSectors: profile === '--engine' ? 196608 : 393216, sectorsPerFat: 256, sectorsPerCluster: profile === '--engine' ? 4 : 8}));
