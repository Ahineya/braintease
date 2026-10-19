import {test} from 'node:test';
import assert from 'node:assert/strict';
import {browserEffects} from './effects.mjs';
import {testPlatform} from './test-platform.mjs';
function batch(memory, records) {
  const words=[0x5246,1,0,records.length,0,0,0,0];
  for(const {kind,id,arg=0,text=''} of records){const bytes=new TextEncoder().encode(text);words.push(8+bytes.length,kind,id,0,arg&65535,arg>>>16,bytes.length,0,...bytes);}
  words[2]=words.length;const view=new DataView(memory.buffer);words.forEach((n,i)=>view.setUint16(i*2,n,true));return words.length;
}
function fixture(hash='#/active') {
  const memory=new WebAssembly.Memory({initial:2}), host=testPlatform(hash), notifications=[];
  const effects=browserEffects({memory:()=>memory,host,notify:()=>notifications.push(1),fatal:assert.fail});
  return {memory,host,effects,notifications,commit:records=>effects.commit(0,batch(memory,records)),
    deliver(){let records=[];effects.deliver(0,()=>{const words=new Uint16Array(memory.buffer);for(let p=4;p<words[2];p+=words[p])records.push({id:words[p+1],kind:words[p+3],status:words[p+4],error:words[p+6],text:new TextDecoder().decode(Uint8Array.from(words.slice(p+8,p+words[p])))});});return records;}};
}
test('location subscriptions snapshot, coalesce, deduplicate navigation and revoke on cancellation',()=>{
  const f=fixture();f.commit([{kind:4,id:1}]);assert.equal(f.effects.pending,1);assert.equal(f.host.listenerCount,1);
  assert.equal(f.deliver()[0].text,'#/active');assert.equal(f.effects.stats.outstandingEffects,1);
  f.commit([{kind:5,id:2,text:'#/completed'}]);assert.deepEqual(f.deliver().map(x=>[x.kind,x.text]),[[4,'#/completed'],[5,'']]);
  f.host.emitHash();assert.equal(f.effects.pending,0);
  f.host.go(-1);assert.equal(f.deliver()[0].text,'#/active');f.host.go(1);assert.equal(f.deliver()[0].text,'#/completed');
  f.host.setHash('#/active');f.host.setHash('#/');assert.equal(f.deliver()[0].text,'#/');
  f.commit([{kind:3,id:1}]);assert.equal(f.host.listenerCount,0);f.host.setHash('#/active');assert.equal(f.effects.pending,0);
  f.effects.dispose();assert.equal(f.effects.stats.outstandingEffects,0);
});
test('platform preflight rejects unsafe navigation and bad storage before any mutation',()=>{
 const f=fixture();for(const text of ['javascript:alert(1)','#/bad path','#/%ZZ','#/?query=1','#/active\n']){
  assert.throws(()=>f.commit([{kind:7,id:1,arg:1,text:'kvalue'},{kind:5,id:2,text}]));assert.equal(f.host.values.size,0);
 }
 assert.throws(()=>f.commit([{kind:7,id:1,arg:0,text:'x'}]));assert.throws(()=>f.commit([{kind:6,id:1,arg:16385,text:'k'}]));
 f.effects.dispose();
});
test('storage distinguishes missing/empty, orders writes, reports quota/unavailability and bounds encoded data',()=>{
 const f=fixture();f.commit([{kind:6,id:1,arg:16384,text:'k'}]);assert.deepEqual(f.deliver()[0],{id:1,kind:6,status:0,error:0,text:''});
 f.commit([{kind:7,id:2,arg:1,text:'k🌍'},{kind:6,id:3,arg:16384,text:'k'}]);assert.equal(f.deliver()[1].text,'🌍');
 f.commit([{kind:7,id:4,arg:1,text:'k'},{kind:6,id:5,arg:16384,text:'k'}]);assert.equal(f.deliver()[1].status,1);
 f.host.storageError='QuotaExceededError';f.commit([{kind:7,id:6,arg:1,text:'kx'}]);assert.equal(f.deliver()[0].error,4);
 f.host.storageError='SecurityError';f.commit([{kind:6,id:7,arg:16384,text:'k'}]);assert.equal(f.deliver()[0].error,3);
 f.host.storageError=null;f.host.values.set('k','🌍'.repeat(4097));f.commit([{kind:6,id:8,arg:16384,text:'k'}]);assert.equal(f.deliver()[0].error,2);
 f.commit([{kind:8,id:9,text:'k'}]);f.deliver();assert.equal(f.host.values.has('k'),false);f.effects.dispose();
});

test('failed listener registration and callbacks racing cancellation release their ownership',()=>{
 const f=fixture();f.host.addEventListener=()=>{throw Error('host registration failed');};
 f.commit([{kind:4,id:1}]);assert.equal(f.effects.stats.locationSubscriptions,0);assert.equal(f.deliver()[0].error,3);f.effects.dispose();
 const next=fixture();let callback;const add=next.host.addEventListener;
 next.host.addEventListener=(name,fn)=>{callback=fn;add(name,fn);};
 next.commit([{kind:4,id:1}]);next.deliver();next.commit([{kind:3,id:1}]);const before=next.notifications.length;
 callback();assert.equal(next.effects.pending,0);assert.equal(next.notifications.length,before);assert.equal(next.host.listenerCount,0);next.effects.dispose();
});

test('platform bounds apply to full batches and UTF-8 byte counts',()=>{
 const f=fixture();assert.throws(()=>f.commit(Array.from({length:65},(_,i)=>({kind:4,id:i+1}))));assert.equal(f.host.listenerCount,0);
 f.commit([{kind:7,id:1,arg:1,text:'k'+'x'.repeat(16384)}]);f.deliver();
 f.commit([{kind:6,id:2,arg:16384,text:'k'}]);assert.equal(f.deliver()[0].text.length,16384);
 assert.throws(()=>f.commit([{kind:7,id:3,arg:1,text:'k'+'x'.repeat(16385)}]));
 assert.throws(()=>f.commit([{kind:7,id:3,arg:1,text:'🌍value'}])); // key split inside UTF-8
 f.effects.dispose();
});
