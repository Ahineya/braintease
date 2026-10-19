// One worker owns one interpreter and its private RAM disk. No shared memory.
import {validateDisk} from './disk.mjs';
let api, running=false, halted=false, timer, generation=0, previousFrame='', deadline=0;
const encoder=new TextEncoder();
const turns=new MessageChannel();let scheduled=0;
turns.port1.onmessage=({data})=>{if(data===scheduled&&running)turn();};
function bytes(ptr,len){return new Uint8Array(api.memory.buffer,ptr,len);}
function errorText(){return new TextDecoder().decode(bytes(api.rvm_error_pointer(),api.rvm_error_length()));}
function upload(data){const p=api.rvm_alloc(data.byteLength);if(!p)throw Error('Not enough memory to load this image');bytes(p,data.byteLength).set(new Uint8Array(data.buffer??data,data.byteOffset??0,data.byteLength));return p;}
function input(fn,data){const p=upload(data);try{if(fn(p,data.byteLength)<0)throw Error(errorText());}finally{api.rvm_dealloc(p,data.byteLength);}}
function schedule(ms=0){clearTimeout(timer);const token=++scheduled;if(!running)return;if(ms>0)timer=setTimeout(()=>turns.port2.postMessage(token),ms);else turns.port2.postMessage(token);}
function turn(){
 if(!running)return;
 try{
  const state=api.rvm_advance(1000000,7);
  if(state<0)throw Error(errorText());
  const count=api.rvm_console_length();
  if(count){const data=bytes(api.rvm_console_pointer(),count).slice();postMessage({type:'console',data:data.buffer},[data.buffer]);}
  const width=api.rvm_frame_width(),height=api.rvm_frame_height();
  const frame=`${width}:${height}:${api.rvm_frame_sequence()}`;
  if(frame!==previousFrame){
   previousFrame=frame;
   if(width&&height){const pixels=bytes(api.rvm_frame_pointer(),width*height*4).slice();postMessage({type:'frame',width,height,pixels:pixels.buffer},[pixels.buffer]);}
   else postMessage({type:'display-off'});
  }
  if(state===2){halted=true;running=false;postMessage({type:'halted'});return;}
  deadline=performance.now()+(state===1?api.rvm_idle_ms():0);
  schedule(Math.max(0,deadline-performance.now()));
 }catch(error){running=false;postMessage({type:'error',message:error.message});}
}
async function boot(importedDisk){
 const current=++generation;running=false;halted=false;clearTimeout(timer);previousFrame='';
 postMessage({type:'status',message:'Starting Ripple…'});
 const diskPromise=importedDisk ? Promise.resolve(importedDisk) : fetch('disk.img.gz').then(r=>{
  if(!r.ok)throw Error('Cannot load clean disk');
  return new Response(r.body.pipeThrough(new DecompressionStream('gzip'))).arrayBuffer();
 });
 const [wasm,boot,disk]=await Promise.all([fetch('rvm.wasm').then(r=>{if(!r.ok)throw Error('Cannot load RVM');return r.arrayBuffer();}),fetch('boot.bin').then(r=>{if(!r.ok)throw Error('Cannot load boot program');return r.arrayBuffer();}),diskPromise]);
 validateDisk(disk);
 const imports={rvm_host:{monotonic_ms:()=>performance.now(),realtime_ms:()=>Date.now(),random_fill:(ptr,len)=>{try{crypto.getRandomValues(new Uint8Array(api.memory.buffer,ptr,len));return 0;}catch{return -1;}}}};
 const module=await WebAssembly.instantiate(wasm,imports);if(current!==generation)return;api=module.instance.exports;
 const bootPointer=upload(boot),diskPointer=upload(disk);
 try{if(api.rvm_boot(bootPointer,boot.byteLength,diskPointer,disk.byteLength)<0)throw Error(errorText());}
 finally{api.rvm_dealloc(bootPointer,boot.byteLength);api.rvm_dealloc(diskPointer,disk.byteLength);}
 running=true;postMessage({type:'ready'});schedule();
}
onmessage=async({data})=>{
 try{
  if(data.type==='boot'){await boot(data.disk);return;}
  if(data.type==='pause'){running=false;clearTimeout(timer);postMessage({type:'paused'});return;}
  if(data.type==='resume'){if(api&&!halted){running=true;schedule();postMessage({type:'resumed'});}return;}
  if(data.type==='export'){
   if(!halted)throw Error('Run shutdown before exporting the disk');
   const disk=bytes(api.rvm_disk_pointer(),api.rvm_disk_length()).slice();postMessage({type:'disk',data:disk.buffer},[disk.buffer]);return;
  }
  if(!api||halted)return;
  if(data.type==='tty')input(api.rvm_tty,encoder.encode(data.text.replace(/\r\n?/g,'\n')));
  if(data.type==='events')input(api.rvm_events,encoder.encode(JSON.stringify(data.events)));
  // Input can wake an idle guest without waiting for its old timeout.
  if(running)schedule();
 }catch(error){postMessage({type:'error',message:error.message});}
};
