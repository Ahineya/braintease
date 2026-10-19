import {checkDiskSize,validateDisk} from './disk.mjs';
const $=id=>document.getElementById(id);
const term=new Terminal({cols:80,rows:30,convertEol:true,cursorBlink:true,fontSize:14,fontFamily:'ui-monospace, SFMono-Regular, Menlo, monospace',scrollback:3000,theme:{background:'#080c0e',foreground:'#d4deda',cursor:'#95cbb4'}});
term.open($('terminal'));
const canvas=$('desktop'),context=canvas.getContext('2d',{alpha:false});
let worker,ready=false,paused=false,graphical=false,view='console',events=[],flushQueued=false,importing=false,booting=false;
const pressed=new Set();
function status(text){$('status').textContent=text;}
function selectView(name){view=name;$('console-view').hidden=name!=='console';$('desktop-view').hidden=name!=='desktop';$('console-tab').classList.toggle('selected',name==='console');$('desktop-tab').classList.toggle('selected',name==='desktop');if(name==='console')term.focus();else canvas.focus();}
function send(message){worker?.postMessage(message);}
function event(event){events.push(event);if(!flushQueued){flushQueued=true;queueMicrotask(()=>{flushQueued=false;while(events.length)send({type:'events',events:events.splice(0,64)});});}}
function diskControls(){$('import').disabled=importing||booting;$('reset').disabled=importing;}
function start(disk){
 booting=true;diskControls();$('disk-error').hidden=true;
 worker?.terminate();ready=false;paused=false;graphical=false;events=[];pressed.clear();term.reset();selectView('console');$('desktop-tab').disabled=true;$('export').disabled=true;$('pause').disabled=true;$('pause').textContent='Pause';status('Loading…');
 const current=new Worker('worker.mjs',{type:'module'});worker=current;
 worker.onmessage=({data})=>{
  if(worker!==current)return;
  if(data.type==='status')status(data.message);
  if(data.type==='ready'){booting=false;diskControls();ready=true;$('pause').disabled=false;status('Running');term.focus();}
  if(data.type==='console')term.write(new Uint8Array(data.data));
  if(data.type==='frame'){
   canvas.width=data.width;canvas.height=data.height;context.putImageData(new ImageData(new Uint8ClampedArray(data.pixels),data.width,data.height),0,0);
   $('desktop-tab').disabled=false;if(!graphical){graphical=true;selectView('desktop');}
  }
  if(data.type==='display-off'){graphical=false;$('desktop-tab').disabled=true;if(view==='desktop')selectView('console');}
  if(data.type==='halted'){ready=false;$('pause').disabled=true;$('export').disabled=false;status('Stopped · disk ready to export');selectView('console');}
  if(data.type==='paused'){paused=true;$('pause').textContent='Resume';status('Paused');}
  if(data.type==='resumed'){paused=false;$('pause').textContent='Pause';status('Running');}
  if(data.type==='error'){booting=false;diskControls();status(data.message);term.writeln('\r\n\x1b[31m'+data.message+'\x1b[0m');}
  if(data.type==='disk'){const url=URL.createObjectURL(new Blob([data.data],{type:'application/octet-stream'}));const a=document.createElement('a');a.href=url;a.download='riplix-browser.img';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}
 };
 worker.onerror=e=>{if(worker!==current)return;booting=false;diskControls();status('Machine stopped: '+e.message);};
 if(disk)worker.postMessage({type:'boot',disk},[disk]);else send({type:'boot'});
}
term.onData(text=>{if(ready)send({type:'tty',text});});
$('console-tab').onclick=()=>selectView('console');$('desktop-tab').onclick=()=>selectView('desktop');
$('pause').onclick=()=>send({type:paused?'resume':'pause'});
$('export').onclick=()=>send({type:'export'});
$('reset').onclick=()=>{if(confirm('Restore the clean disk? Changes in this tab will be discarded.'))start();};
$('import').onclick=()=>$('disk-file').click();
$('disk-file').onchange=async()=>{
 const file=$('disk-file').files[0];$('disk-file').value='';
 if(!file||importing||booting)return;
 importing=true;diskControls();$('disk-error').hidden=true;
 try{
  checkDiskSize(file.size);
  const disk=await file.arrayBuffer();validateDisk(disk);
  if(confirm('Replace the disk in this tab? To keep its current files, cancel, run shutdown, and export the disk first.'))start(disk);
 }catch(error){$('disk-error').textContent=error.message;$('disk-error').hidden=false;}
 finally{importing=false;diskControls();}
};
function hid(code){
 if(/^Key[A-Z]$/.test(code))return code.charCodeAt(3)-65+4;
 if(/^Digit[1-9]$/.test(code))return +code[5]+29;if(code==='Digit0')return 39;
 if(/^F([1-9]|1[0-2])$/.test(code))return +code.slice(1)+57;
 return {Enter:40,Escape:41,Backspace:42,Tab:43,Space:44,Minus:45,Equal:46,BracketLeft:47,BracketRight:48,Backslash:49,Semicolon:51,Quote:52,Backquote:53,Comma:54,Period:55,Slash:56,CapsLock:57,PrintScreen:70,ScrollLock:71,Pause:72,Insert:73,Home:74,PageUp:75,Delete:76,End:77,PageDown:78,ArrowRight:79,ArrowLeft:80,ArrowDown:81,ArrowUp:82,ControlLeft:224,ShiftLeft:225,AltLeft:226,MetaLeft:227,ControlRight:228,ShiftRight:229,AltRight:230,MetaRight:231}[code];
}
canvas.addEventListener('keydown',e=>{const key=hid(e.code);if(key===undefined||!ready)return;e.preventDefault();if(!pressed.has(key)){pressed.add(key);event({type:'key',key,down:true});}});
window.addEventListener('keyup',e=>{const key=hid(e.code);if(pressed.delete(key)){e.preventDefault();event({type:'key',key,down:false});}});
function release(){if(pressed.size){for(const key of pressed)event({type:'key',key,down:false});pressed.clear();}event({type:'focus',focused:false});}
canvas.addEventListener('blur',release);window.addEventListener('blur',release);
canvas.addEventListener('focus',()=>event({type:'focus',focused:true}));
function point(e){const r=canvas.getBoundingClientRect();return{x:Math.round((e.clientX-r.left)*canvas.width/r.width),y:Math.round((e.clientY-r.top)*canvas.height/r.height)};}
canvas.addEventListener('pointermove',e=>event({type:'motion',...point(e)}));
const button=n=>n===1?2:n===2?1:n;
canvas.addEventListener('pointerdown',e=>{e.preventDefault();canvas.focus();canvas.setPointerCapture(e.pointerId);event({type:'button',button:button(e.button),down:true,...point(e)});});
canvas.addEventListener('pointerup',e=>{e.preventDefault();event({type:'button',button:button(e.button),down:false,...point(e)});if(canvas.hasPointerCapture(e.pointerId))canvas.releasePointerCapture(e.pointerId);});
canvas.addEventListener('pointercancel',()=>{event({type:'capture',captured:false});release();});
canvas.addEventListener('pointerenter',()=>event({type:'enter',inside:true}));canvas.addEventListener('pointerleave',()=>event({type:'enter',inside:false}));
canvas.addEventListener('contextmenu',e=>e.preventDefault());
canvas.addEventListener('wheel',e=>{e.preventDefault();const unit=e.deltaMode===1?1:e.deltaMode===2?30:1/40;event({type:'wheel',x:Math.round(e.deltaX*unit*65536),y:Math.round(-e.deltaY*unit*65536)});},{passive:false});
start();
