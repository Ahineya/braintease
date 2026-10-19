// Requires the dedicated local site server; reads only the clean smoke export.
import fs from 'node:fs';
import {Worker} from 'node:worker_threads';
import {validateDisk} from '../disk.mjs';
const base=process.argv[2]??'http://127.0.0.1:8086/';
let worker,log='',halted=false,exported,error;
const prompt='\x1b[1;32mroot@ripple\x1b[0m \x1b[1;36m~\x1b[0m \x1b[1;33m#\x1b[0m ';
async function until(predicate){const end=Date.now()+60000;while(!predicate()){
 if(error)throw error;if(Date.now()>end)throw Error('worker timeout: '+log.slice(-3000));
 await new Promise(r=>setTimeout(r,10));
}}
async function boot(disk){
 if(worker)await worker.terminate();log='';halted=false;exported=undefined;error=undefined;
 worker=new Worker(new URL('./worker-node.mjs',import.meta.url),{workerData:{base}});
 const decoder=new TextDecoder();worker.on('error',e=>error=e);
 worker.on('message',m=>{if(m.type==='console')log+=decoder.decode(m.data,{stream:true});
  if(m.type==='error')error=Error(m.message);if(m.type==='halted')halted=true;if(m.type==='disk')exported=m.data;});
 if(disk)worker.postMessage({type:'boot',disk},[disk]);else worker.postMessage({type:'boot'});
 await until(()=>log.includes(prompt));
}
async function shell(text){const at=log.length;worker.postMessage({type:'tty',text:text+'\r'});
 await until(()=>log.slice(at).includes(prompt));return log.slice(at);}
try{
 const input=fs.readFileSync(new URL('../../../build/rvm-web/tests/export.img',import.meta.url));
 validateDisk(input);await boot(input.buffer.slice(input.byteOffset,input.byteOffset+input.byteLength));
 const previous=await shell('cat /tmp/rift-note');if(!previous.includes('wasm edit saved'))throw Error('previous export lost its file');
 await shell('echo IMPORT_REOPEN_OK > /import-check');
 worker.postMessage({type:'tty',text:'shutdown\r'});await until(()=>halted);
 worker.postMessage({type:'export'});await until(()=>exported);validateDisk(exported);
 await boot(exported);const reopened=await shell('cat /import-check');
 if(!reopened.includes('IMPORT_REOPEN_OK'))throw Error('export/reimport lost new contents');
 await boot();const clean=await shell('test -f /import-check; echo SEED_STATUS:$?');
 if(!clean.includes('SEED_STATUS:1'))throw Error('reset did not restore the bundled seed');
 console.log('PASS: real worker import, original file, write, shutdown/export, reimport and reset to seed');
}finally{if(worker)await worker.terminate();}
