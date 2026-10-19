// Deterministic browser services for wire and compiled-Wasm integration tests.
export function testPlatform(initial = '#/') {
  let history=[initial],at=0,next=1,now=0;
  const listeners=new Set(),timers=new Map(),values=new Map();
  const host={values,timers,storageError:null,storageWrites:0,
    location:{get hash(){return history[at];}},
    history:{pushState(_state,_title,hash){history.splice(++at);history.push(hash);},replaceState(_state,_title,hash){history[at]=hash;}},
    addEventListener(name,fn){if(name==='hashchange')listeners.add(fn);},
    removeEventListener(name,fn){if(name==='hashchange')listeners.delete(fn);},
    get listenerCount(){return listeners.size;},
    emitHash(){for(const fn of [...listeners])fn();},
    setHash(hash){host.history.pushState(null,'',hash);host.emitHash();},
    go(distance){at=Math.max(0,Math.min(history.length-1,at+distance));host.emitHash();},
    now:()=>now,
    setTimeout(fn,delay){const id=next++;timers.set(id,{fn,delay});return id;},
    clearTimeout(id){timers.delete(id);},
    step(){const first=timers.entries().next().value;if(!first)return false;const[id,t]=first;timers.delete(id);now+=t.delay;t.fn();return true;},
    flush(){for(let i=0;host.step();++i)if(i>256)throw Error('non-idle browser task loop');},
  };
  function available(){if(host.storageError){const e=Error(host.storageError);e.name=host.storageError;throw e;}}
  host.localStorage={getItem(key){available();return values.get(key)??null;},setItem(key,value){available();++host.storageWrites;values.set(key,value);},removeItem(key){available();values.delete(key);}};
  return host;
}
