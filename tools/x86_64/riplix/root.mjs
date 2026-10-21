import {rootImage, atomicWrite} from '../../../ports/fuzix/tools/disk.mjs';
import {installRoot, growRoot} from '../../../ports/fuzix/tools/install.mjs';
import fs from 'node:fs';
import path from 'node:path';
import {openImage} from '../../../ports/fuzix/tools/image.mjs';
import {scheduleFiles} from '../../../ports/fuzix/tools/schedule.mjs';
import {manualFiles} from '../../../ports/fuzix/tools/manuals.mjs';
import {sokobanFiles} from '../../../ports/fuzix/tools/sokoban.mjs';
import {systemFiles} from './system.mjs';
import {ptyDevices} from '../../../ports/fuzix/tools/pty.mjs';
import {basicFiles} from '../../../ports/basic/files.mjs';
const [output, applications, ...options] = process.argv.slice(2);
const offline = options.includes('--offline'), network = options.includes('--network'), upgrade = options.indexOf('--upgrade');
const sshAt=options.indexOf('--ssh-tools'), keyAt=options.indexOf('--ssh-authorized-key');
const sshTools=sshAt>=0?options[sshAt+1]:null, sshKey=keyAt>=0?options[keyAt+1]:null;
const services = options.includes('--system-services'), sizeAt = options.indexOf('--root-mib');
const mib = sizeAt >= 0 ? Number(options[sizeAt + 1]) : null;
if (sizeAt >= 0 && (!Number.isInteger(mib) || mib < 16 || mib > 1536)) throw Error('root size must be16..1536 MiB');
const wifiTest = options.includes('--wifi-test'), configAt = options.indexOf('--network-config');
const configPath = configAt >= 0 ? options[configAt + 1] : null;
const previous = upgrade >= 0 ? options[upgrade + 1] : null;
if (options.some((o, i) => o !== '--ssh-tools' && o !== '--ssh-authorized-key' && !(sshAt>=0 && i===sshAt+1) && !(keyAt>=0 && i===keyAt+1) && o !== '--system-services' && o !== '--root-mib' && !(sizeAt >= 0 && i === sizeAt + 1) && o !== '--offline' && o !== '--network' && o !== '--upgrade' && o !== '--wifi-test' && o !== '--network-config' && !(configAt >= 0 && i === configAt + 1) && !(upgrade >= 0 && i === upgrade + 1)) || (upgrade >= 0 && !previous) || (configAt >= 0 && !configPath))
  throw Error('usage: root.mjs OUTPUT APPLICATIONS [--offline] [--network] [--wifi-test] [--network-config JSON] [--upgrade OLD_ROOT]');
if ((wifiTest || configPath) && !network) throw Error('network configuration requires --network');
if (wifiTest && !configPath) throw Error('live Wi-Fi requires explicit IPv4 configuration');
let config = null;
if (configPath) {
  config = JSON.parse(fs.readFileSync(configPath, 'utf8'));
  const keys = ['address', 'netmask', 'gateway', 'dns'];
  if (Object.keys(config).length !== keys.length || !keys.every(k => typeof config[k] === 'string'))
    throw Error('network config requires address, netmask, gateway and dns');
  const ip = value => {
    if (!/^(0|[1-9][0-9]{0,2})(\.(0|[1-9][0-9]{0,2})){3}$/.test(value)) throw Error('invalid IPv4 config');
    const a = value.split('.').map(Number);
    if (a.some(v => v > 255)) throw Error('invalid IPv4 octet');
    return a.reduce((v, b) => (v * 256 + b) >>> 0, 0);
  };
  const [a, m, g, d] = keys.map(k => ip(config[k]));
  const host = (~m) >>> 0;
  if (!m || !host || ((host + 1) & host) || host < 3 ||
      [a, g, d].some(v => (v >>> 24) === 0 || (v >>> 24) === 127 || (v >>> 24) >= 224) ||
      (a & m) !== (g & m) || !(a & host) || (a & host) === host || !(g & host) || (g & host) === host || a === g)
    throw Error('invalid IPv4 subnet/host/gateway');
}
if (!output) throw Error('usage: root.mjs OUTPUT [APPLICATIONS]');
if (previous) {
  const source = fs.lstatSync(previous);
  if (!source.isFile() || source.isSymbolicLink() || path.resolve(previous) === path.resolve(output))
    throw Error('upgrade requires a separate regular backup image');
}
let target;
try { target = fs.lstatSync(output); }
catch (error) { if (error.code !== 'ENOENT') throw error; }
if (target && (!target.isFile() || target.isSymbolicLink()))
  throw Error('output must be a regular image');
// Small bootstrap manifests remain useful for kernel acceptance checks.
// The normal boot installs independent RXE1 programs over their old manifests.
const files = ['init', 'sh', 'hello', 'check', 'cat', 'ls', 'pipecheck'].map(name => ({
  name: name === 'init' ? '/init' : `/bin/${name}`,
  mode: 0o100755, data: Buffer.from(`FXN1:${name}:1\n`),
}));
files.push({name: '/etc/passwd', mode: 0o100644, preserve: true, data: Buffer.from('root:x:0:0:root:/root:/bin/sh\n')});
if (applications) {
  const aliases = {crond: 'cron', mv: 'cp', ln: 'cp', tcl: 'picol', tclsh: 'picol', sokoban: 'sok'};
  const applicationNames=JSON.parse(fs.readFileSync(path.join(applications, 'manifest.json'), 'utf8'));
  for (const name of applicationNames)
    files.push({name: `/bin/${name}`, mode: 0o100755, ...(name==='crond' ? {symlink:'cron'} : aliases[name] ? {link: `/bin/${aliases[name]}`} : {data: fs.readFileSync(path.join(applications, `${name}.fxe`))})});
  if(applicationNames.includes('cron')) files.push(...scheduleFiles());
  if(applicationNames.includes('test')) files.push({name:'/bin/[',mode:0o100755,link:'/bin/test'});
  files.push({name: '/dev/null', mode: 0o20666, device: 0x100});
  files.push({name: '/dev/tty', mode: 0o20666, device: 0x200});
  files.push({name: '/demo.sh', mode: 0o100755, data: Buffer.from(
    'for word in independently loaded apps; do echo "$word"; done\n' +
    'echo pipeline | cat > /tmp/demo\ncat /tmp/demo\n' +
    'if test -s /tmp/demo; then echo BOURNE_SCRIPT_OK; fi\n')});
}
if (network) {
  if (!offline) files.push(...ptyDevices());
  files.push({name:'/etc/resolv.conf', mode:0o100644, preserve:true,
              data:Buffer.from(`nameserver ${config ? config.dns : '192.168.64.3'}\n`)});
  files.push({name:'/network.sh', mode:0o100755,
              data:Buffer.from(config ?
                `ifconfig eth0 ${config.address} netmask ${config.netmask} gateway ${config.gateway} up || exit 1\n` +
                `if ping -c 3 ${config.gateway}; then\n  echo AX211_IP_OK\n  telnetd &\n  echo Telnet-ready ${config.address} port 23\nelse\n  echo AX211_IP_FAILED\n  exit 1\nfi\n` :
                'ifconfig eth0\ntelnetd &\n')});
}
if (offline) {
  files.push({name:'/offline-check.sh',mode:0o100755,
              data:fs.readFileSync(new URL('../tests/offline.sh',import.meta.url))});
  files.push({name:'/dev/root',mode:0o60600,device:0});
  files.push(...ptyDevices(), ...manualFiles(), ...sokobanFiles(), ...basicFiles());
  for (const [name, device] of [['zero',0x102],['proc',0x103]])
    files.push({name:`/dev/${name}`, mode:0o20666, device});
  for (const [name, text] of [
    ['group','root:x:0:root\n'], ['mtab','/dev/root / riplix rw 0 0\n'],
    ['termcap','ansi|vt100|xterm|ripple:co#80:li#24:cl=\\E[H\\E[2J:cm=\\E[%i%d;%dH:ce=\\E[K:up=\\E[A:bc=^H:\n']])
    files.push({name:`/etc/${name}`, mode:0o100644, preserve:true, data:Buffer.from(text)});
  for (const name of ['makeactions','makemacros'])
    files.push({name:`/usr/lib/${name}`, mode:0o100644,
                data:fs.readFileSync(new URL(`../../../ports/fuzix/upstream/Applications/MWC/cmd/${name}`,import.meta.url))});
  files.push({name:'/hello.tcl', mode:0o100644, preserve:true,
              data:fs.readFileSync(new URL('../../../ports/fuzix/examples/hello.tcl',import.meta.url))});
}
if (Boolean(sshTools)!==Boolean(sshKey) || (sshTools && !services)) throw Error('SSH requires tools/key and service profile');
if (services) files.push(...systemFiles(config,sshTools,sshKey,wifiTest));
const blocks = mib !== null ? mib * 512 : offline ? 65536 : applications ? 8192 : 2048;
const inodes = services ? 8192 : offline ? 1024 : 128;
const root = previous ? growRoot(openImage(previous), blocks, inodes) : rootImage([], blocks, inodes);
files.push({name:'/README',mode:0o100644,data:Buffer.from(
  'Welcome to freestanding Riplix on x86_64.\nUse shutdown to flush files before stopping the machine.\n' +
  (offline ? 'Offline Unix apps installed; try man, awk, basic, picol, ed, ps and df.\n' :
             'Bourne shell and separate apps: try help, ls /bin, sh /demo.sh, appcheck, shutdown.\n'))});
atomicWrite(output, installRoot(root, files));
