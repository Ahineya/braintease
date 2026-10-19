// Real Wasm + real clean FUZIX image. Node supplies only clocks and entropy.
import fs from 'node:fs';import path from 'node:path';import zlib from 'node:zlib';import {randomFillSync} from 'node:crypto';import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../../..');
const site=path.resolve(process.argv[2]??path.join(root,'build/rvm-web/site'));
let api;
const {instance}=await WebAssembly.instantiate(fs.readFileSync(path.join(site,'rvm.wasm')),{rvm_host:{monotonic_ms:()=>performance.now(),realtime_ms:()=>Date.now(),random_fill:(p,n)=>{randomFillSync(new Uint8Array(api.memory.buffer,p,n));return 0;}}});api=instance.exports;
const bytes=(p,n)=>new Uint8Array(api.memory.buffer,p,n);
const error=()=>new TextDecoder().decode(bytes(api.rvm_error_pointer(),api.rvm_error_length()));
function upload(data){const p=api.rvm_alloc(data.length);if(!p)throw Error('allocation failed');bytes(p,data.length).set(data);return p;}
function call(fn,data){const p=upload(data);try{if(fn(p,data.length)<0)throw Error(error());}finally{api.rvm_dealloc(p,data.length);}}
const boot=fs.readFileSync(path.join(site,'boot.bin')),disk=zlib.gunzipSync(fs.readFileSync(path.join(site,'disk.img.gz')));
const b=upload(boot),d=upload(disk);if(api.rvm_boot(b,boot.length,d,disk.length)<0)throw Error(error());api.rvm_dealloc(b,boot.length);api.rvm_dealloc(d,disk.length);
const prompt='\x1b[1;32mroot@ripple\x1b[0m \x1b[1;36m~\x1b[0m \x1b[1;33m#\x1b[0m ';
let log='',cursor=0;const decoder=new TextDecoder();const started=performance.now();
function advance(n,ms){const state=api.rvm_advance(n,ms);if(state<0)throw Error(error());log+=decoder.decode(bytes(api.rvm_console_pointer(),api.rvm_console_length()),{stream:true});return state;}
async function until(predicate,seconds=90){const end=performance.now()+seconds*1000;while(!predicate()){
 const state=advance(1000000,7);
 if(performance.now()>end)throw Error('guest timed out: '+log.slice(-4000));
 if(state===1)await new Promise(r=>setTimeout(r,api.rvm_idle_ms()));
 if(state===2&&!predicate())throw Error('guest halted too soon: '+log.slice(-2000));
}}
async function shell(command){cursor=log.length;call(api.rvm_tty,new TextEncoder().encode(command+'\n'));await until(()=>log.slice(cursor).includes(prompt));return log.slice(cursor);}
await until(()=>log.includes(prompt));
if(api.rvm_frame_width()!==0)throw Error('desktop opened automatically');
console.log('Console ready:',Math.round(performance.now()-started),'ms');
const check=await shell('echo WEB_ROUNDTRIP > /web-test; cat /web-test; echo FILE_STATUS:$?');if(!check.includes('WEB_ROUNDTRIP')||!check.includes('FILE_STATUS:0'))throw Error(check);
const tools=await shell("seq 1 5 | awk '{s+=$1} END {print s}'; echo TOOL_STATUS:$?");
if(!tools.includes('15')||!tools.includes('TOOL_STATUS:0'))throw Error('Unix tools failed: '+tools);
const muxHelp=await shell('mux --help');if(!muxHelp.includes('Ctrl-B'))throw Error('mux missing: '+muxHelp);
cursor=log.length;call(api.rvm_tty,new TextEncoder().encode('mux sh\n'));
await until(()=>log.slice(cursor).includes('\x1b[?1049h'));
call(api.rvm_tty,new Uint8Array([2,63]));await until(()=>log.slice(cursor).includes('mux - tabs, tiled panes and persistent sessions'));
call(api.rvm_tty,new TextEncoder().encode('qecho MUX_WASM > /tmp/mux-note\nexit\n'));
await until(()=>log.slice(cursor).includes('\x1b[?1049l'),120);
await until(()=>log.slice(cursor).includes(prompt));
const muxFile=await shell('cat /tmp/mux-note');if(!muxFile.includes('MUX_WASM'))throw Error('mux shell failed: '+muxFile);
console.log('Mux session, help, child shell and exit passed');
cursor=log.length;call(api.rvm_tty,new TextEncoder().encode('rift /tmp/rift-note\n'));
await until(()=>log.slice(cursor).includes('NORMAL'),180);
call(api.rvm_tty,new TextEncoder().encode('iwasm edit saved'));
await until(()=>log.slice(cursor).includes('INSERT'),60);
call(api.rvm_tty,new Uint8Array([19]));await until(()=>log.slice(cursor).includes('Saved /tmp/rift-note'),90);
cursor=log.length;call(api.rvm_tty,new Uint8Array([17]));await until(()=>log.slice(cursor).includes(prompt),90);
const edited=await shell('cat /tmp/rift-note; echo RIFT_STATUS:$?');if(!edited.includes('wasm edit saved')||!edited.includes('RIFT_STATUS:0'))throw Error('Rift save failed: '+edited);
console.log('Rift editing, save and exit passed');
cursor=log.length;call(api.rvm_tty,new TextEncoder().encode('mc /tmp /tmp\n'));await until(()=>log.slice(cursor).includes('RFL Commander'),180);
cursor=log.length;call(api.rvm_tty,new TextEncoder().encode('q'));await until(()=>log.slice(cursor).includes(prompt),90);
const commander=await shell('echo MC_STATUS:$?');if(!commander.includes('MC_STATUS:0'))throw Error('Commander failed: '+commander);
console.log('Commander startup and exit passed');
const bad=new TextEncoder().encode('[{"type":"key","key":999,"down":true}]');const badPointer=upload(bad);if(api.rvm_events(badPointer,bad.length)!==-1)throw Error('invalid key accepted');api.rvm_dealloc(badPointer,bad.length);
cursor=log.length;call(api.rvm_tty,new TextEncoder().encode((process.env.RVM_WEB_TRACE?'NANOX_TRACE_WIRE=1 ':'')+'NANOX_TRACE=1 startx\n'));
await until(()=>log.slice(cursor).includes('NXDSKTOP_READY ')&&log.slice(cursor).includes('NXTERM_READY ')&&api.rvm_frame_width()===640&&bytes(api.rvm_frame_pointer()+(470*640+5)*4,3).every((v,i)=>v===[49,48,49][i]),120);
console.log('Desktop ready:',Math.round(performance.now()-started),'ms; frame',api.rvm_frame_width(),api.rvm_frame_height());
function physical(text){const events=[];for(const c of text){let key,shift=false;if(/[a-z]/.test(c))key=c.charCodeAt(0)-97+4;else if(c===' ')key=44;else if(c==='-')key=45;else if(c==='/')key=56;else if(c===';')key=51;else if(c==='>'){key=55;shift=true;}else if(c==='\n')key=40;else throw Error('unsupported test key '+c);if(shift)events.push({type:'key',key:225,down:true});events.push({type:'key',key,down:true},{type:'key',key,down:false});if(shift)events.push({type:'key',key:225,down:false});}return events;}
async function settle(ms){const end=performance.now()+ms;while(performance.now()<end){advance(1000000,7);await new Promise(r=>setTimeout(r,1));}}
call(api.rvm_events,new TextEncoder().encode(JSON.stringify([{type:'focus',focused:true},{type:'motion',x:100,y:100},{type:'button',button:0,down:true,x:100,y:100},{type:'button',button:0,down:false,x:100,y:100}])));await settle(300);
for(const e of physical('echo gui-ok > /gui-test; echo gui-done > /dev/console\n')){call(api.rvm_events,new TextEncoder().encode(JSON.stringify([e])));if(!e.down&&e.key!==225)await settle(35);}
// Let the terminal shell consume the ordered events, then leave the desktop.
await until(()=>log.includes('gui-done'),90);
fs.mkdirSync(path.join(root,'build/rvm-web/tests'),{recursive:true});fs.writeFileSync(path.join(root,'build/rvm-web/tests/gui.rgba'),bytes(api.rvm_frame_pointer(),api.rvm_frame_width()*api.rvm_frame_height()*4));
cursor=log.length;call(api.rvm_tty,new Uint8Array([3]));await until(()=>log.slice(cursor).includes(prompt),60);
const gui=await shell('cat /gui-test; echo GUI_STATUS:$?');fs.writeFileSync(path.join(root,'build/rvm-web/tests/guest.log'),log);if(!gui.includes('gui-ok')||!gui.includes('GUI_STATUS:0'))throw Error('graphical terminal input failed: '+gui);
cursor=log.length;call(api.rvm_tty,new TextEncoder().encode('shutdown\n'));await until(()=>log.slice(cursor).includes('power off.'),30);
let stopped=false;await until(()=>stopped||(stopped=advance(1000000,7)===2),30);
fs.mkdirSync(path.join(root,'build/rvm-web/tests'),{recursive:true});fs.writeFileSync(path.join(root,'build/rvm-web/tests/guest.log'),log);
fs.writeFileSync(path.join(root,'build/rvm-web/tests/export.img'),bytes(api.rvm_disk_pointer(),api.rvm_disk_length()));
console.log('PASS: console-first boot, file I/O, Unix tools, mux, Rift editing, Commander, invalid input rejection, manual desktop, graphical terminal typing, return to console, shutdown.');
