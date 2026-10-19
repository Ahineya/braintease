import {test} from 'node:test';
import assert from 'node:assert/strict';
import {taskPump} from './task-pump.mjs';
function fixture() {
  let now=0,next=1,results=0,alive=true,turns=0,deliveries=0;
  const timers=new Map(),errors=[];
  let onTurn=()=>{},onDeliver=()=>{};
  const pump=taskPump({host:{now:()=>now,setTimeout(fn,ms){const id=next++;timers.set(id,{fn,ms});return id;},clearTimeout(id){timers.delete(id);}},
    active:()=>alive,pending:()=>results,deliver(){++deliveries;results=0;onDeliver();},turn(){++turns;onTurn();},onError:e=>errors.push(e)});
  return {pump,timers,errors,advance(ms){now+=ms;},clock(value){now=value;},results(n){results=n;pump.request();},
    onTurn(fn){onTurn=fn;},onDeliver(fn){onDeliver=fn;},get turns(){return turns;},get deliveries(){return deliveries;},
    fire(){const [id,t]=timers.entries().next().value;timers.delete(id);now+=t.ms;t.fn();},close(){alive=false;pump.dispose();}};
}

test('self-wakes run one turn per browser task and None leaves no idle polling',()=>{
 const f=fixture();f.onTurn(()=>{if(f.turns<4)f.pump.hint(0);else f.pump.hint(4294967295);});
 f.pump.hint(0);assert.equal(f.turns,0);
 for(let i=1;i<=4;++i){f.fire();assert.equal(f.turns,i);assert.equal(f.timers.size,i===4?0:1);}
 f.pump.drain();assert.equal(f.turns,4);assert.equal(f.pump.stats.executorTurns,4);f.close();
});
test('completions preempt a later timer and dispatch one bounded guest entry',()=>{
 const f=fixture();f.pump.hint(100);f.results(3);f.results(4);
 assert.equal(f.timers.size,1);assert.equal([...f.timers.values()][0].ms,0);
 f.onDeliver(()=>f.pump.hint(100));f.fire();
 assert.equal(f.deliveries,1);assert.equal(f.turns,0);assert.equal(f.timers.size,1);
 f.onTurn(()=>f.pump.hint(4294967295));f.fire();assert.equal(f.turns,1);assert.equal(f.timers.size,0);f.close();
});
test('clock wraps only at the guest boundary; invalid clocks fail explicitly',()=>{
 const f=fixture();f.clock(4294967290);assert.equal(f.pump.sample(),4294967290);
 f.advance(10);assert.equal(f.pump.sample(),4);
 f.clock(1);assert.throws(()=>f.pump.sample(),/monotonic/);f.close();
 for(const value of [-1,NaN,Infinity,Number.MAX_SAFE_INTEGER+1]) {
   const f=fixture();f.clock(value);assert.throws(()=>f.pump.sample(),/monotonic/);f.close();
 }
});
test('disposal cancels scheduled tasks and stale callbacks cannot resume the guest',()=>{
 const f=fixture();f.pump.hint(0);const stale=[...f.timers.values()][0].fn;
 f.close();assert.equal(f.timers.size,0);stale();assert.equal(f.turns,0);assert.equal(f.deliveries,0);
 f.pump.hint(0);f.pump.request();assert.equal(f.timers.size,0);
});
