import {test} from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {mountBrowser} from './bridge.mjs';
import {testDOM} from './test-dom.mjs';
import {testPlatform} from './test-platform.mjs';
const wasm=readFileSync(process.env.RIPPLE_TODO_WASM);
const KEY='rfl.todomvc.v1';
function nodes(root,predicate){const out=[];function walk(n){if(predicate(n))out.push(n);for(const child of n.childNodes)walk(child);}walk(root);return out;}
function klass(root,name){return nodes(root,n=>(n.attributes.get('class')??'').split(' ').includes(name));}
async function mounted(host=testPlatform()){
 const dom=testDOM(),errors=[];const app=await mountBrowser(wasm,{...dom,effectHost:host,onError:e=>errors.push(e)});
 const flush=()=>{for(let i=0;i<40&&(host.timers.size||dom.frames.size);++i){host.flush();dom.frame();}assert.deepEqual(errors,[]);};
 const find=name=>klass(dom.root,name)[0];
 const add=title=>{const input=find('new-todo');input.value=title;input.dispatch('input');input.dispatch('keydown',{key:'Enter'});dom.frame();flush();};
 const click=action=>{const n=nodes(dom.root,n=>n.attributes.get('data-action')===action)[0];assert.ok(n,action);n.dispatch('click');dom.frame();flush();};
 const dispose=()=>{app.dispose();assert.equal(host.listenerCount,0);assert.equal(host.timers.size,0);assert.equal(app.stats.locationSubscriptions,0);assert.equal(app.stats.outstandingEffects,0);assert.equal(app.stats.liveAllocations,1);assert.equal(app.stats.liveNodes,0);assert.deepEqual(errors,[]);};
 return {dom,host,app,flush,find,add,click,dispose,rows:()=>find('todo-list')?.childNodes??[]};
}
test('TodoMVC edits, routes, history, persistence and disposal use the actual compiled app',async()=>{
 assert.deepEqual(WebAssembly.Module.imports(new WebAssembly.Module(wasm)).map(i=>i.module+'.'+i.name).sort(),['ripple.dom_commit','ripple.effect_commit','ripple.write']);
 const t=await mounted();assert.equal(t.host.storageWrites,0);assert.equal(t.find('new-todo').disabled,true);t.flush();assert.equal(t.host.storageWrites,0);
 assert.equal(t.dom.document.activeElement,t.find('new-todo'));assert.equal(t.app.stats.locationSubscriptions,1);assert.equal(t.app.stats.scheduledTask,false);
 t.add('  first 🌍  ');t.add('second');assert.equal(t.rows().length,2);assert.equal(t.find('new-todo').value,'');assert.equal(t.host.storageWrites,2);
 const first=t.rows()[0],second=t.rows()[1],label=nodes(first,n=>n.tagName==='label')[0];
 label.dispatch('dblclick');t.dom.frame();let edit=klass(first,'edit')[0];assert.equal(t.dom.document.activeElement,edit);
 edit.value='updated';edit.dispatch('input');edit.dispatch('keydown',{key:'Enter'});edit.dispatch('blur');t.dom.frame();t.flush();assert.equal(label.textContent,'updated');assert.equal(t.rows()[1],second);assert.equal(t.host.storageWrites,3);
 label.dispatch('dblclick');t.dom.frame();edit=klass(first,'edit')[0];edit.value='cancelled';edit.dispatch('input');edit.dispatch('keydown',{key:'Escape'});edit.dispatch('blur');t.dom.frame();t.flush();assert.equal(label.textContent,'updated');assert.equal(t.host.storageWrites,3);
 label.dispatch('dblclick');t.dom.frame();edit=klass(first,'edit')[0];edit.value='';edit.dispatch('input');edit.dispatch('keydown',{key:'Enter',isComposing:true});t.dom.frame();assert.ok(klass(first,'edit')[0]);edit.dispatch('keydown',{key:'Escape'});t.dom.frame();t.flush();
 const toggle=klass(first,'toggle')[0];toggle.checked=true;toggle.dispatch('change');t.dom.frame();t.flush();assert.ok(first.attributes.get('class').includes('completed'));
 const unchanged=t.app.stats;t.host.setHash('#/active');t.flush();assert.equal(t.rows().length,1);assert.equal(t.rows()[0],second);assert.equal(t.host.storageWrites,4);
 const entries=t.app.stats.guestEntries;t.host.emitHash();t.flush();assert.equal(t.app.stats.guestEntries,entries);
 t.host.go(-1);t.flush();assert.equal(t.rows().length,2);assert.equal(t.rows()[1],second);
 t.host.go(1);t.flush();assert.equal(t.rows().length,1);t.host.setHash('#/completed');t.flush();assert.equal(t.rows().length,1);
 t.find('clear-completed').dispatch('click');t.dom.frame();t.flush();assert.equal(t.rows().length,0);t.host.setHash('#/');t.flush();assert.equal(t.rows().length,1);
 const stored=t.host.values.get(KEY);t.dispose();
 const reloaded=await mounted(t.host);reloaded.flush();assert.equal(reloaded.rows().length,1);assert.ok(reloaded.dom.root.textContent.includes('second'));assert.equal(t.host.values.get(KEY),stored);
 reloaded.find('toggle-all').checked=true;reloaded.find('toggle-all').dispatch('change');reloaded.dom.frame();reloaded.flush();reloaded.find('clear-completed').dispatch('click');reloaded.dom.frame();reloaded.flush();assert.equal(reloaded.find('main'),undefined);reloaded.dispose();
});
test('malformed data is not overwritten; explicit reset and storage retry recover',async()=>{
 const host=testPlatform('#/unknown');host.values.set(KEY,'corrupt data');const t=await mounted(host);t.flush();
 assert.equal(host.location.hash,'#/');assert.equal(t.find('new-todo').disabled,true);assert.equal(host.values.get(KEY),'corrupt data');assert.equal(host.storageWrites,0);
 t.click('retry-load');assert.equal(host.storageWrites,0);assert.equal(host.values.get(KEY),'corrupt data');
 t.click('start-empty');assert.equal(t.find('new-todo').disabled,false);assert.equal(host.storageWrites,1);
 host.storageError='QuotaExceededError';t.add('retained in memory');assert.equal(t.rows().length,1);assert.ok(t.dom.root.textContent.includes('quota'));
 host.storageError=null;t.click('retry-save');assert.ok(host.values.get(KEY).includes('retained in memory'));t.dispose();
});
test('same-turn edits serialize saves, keep unchanged renders quiet, and cancel pending work',async()=>{
 const t=await mounted();t.flush();const input=t.find('new-todo');
 for(const title of ['a','b','c']){input.value=title;input.dispatch('input');input.dispatch('keydown',{key:'Enter'});}t.dom.frame();
 assert.equal(t.rows().length,3);t.flush();assert.equal(t.host.storageWrites,2);assert.ok(t.host.values.get(KEY).includes('c'));
 const before=t.app.stats;input.dispatch('keydown',{key:'Shift'});t.dom.frame();t.flush();assert.equal(t.app.stats.commits,before.commits);assert.equal(t.app.stats.hostCalls,before.hostCalls);
 const row=t.rows()[0],label=nodes(row,n=>n.tagName==='label')[0];label.dispatch('dblclick');t.dom.frame();const edit=klass(row,'edit')[0];edit.value='  ';edit.dispatch('blur');t.dom.frame();assert.equal(t.rows().length,2);
 t.dispose();
});


test('unavailable initial storage keeps existing data intact until a successful retry',async()=>{
 const host=testPlatform('#/active');const saved='RFLTODO1\n1\n7\n0\n6\nstored\n';host.values.set(KEY,saved);host.storageError='SecurityError';
 const t=await mounted(host);t.flush();assert.equal(t.find('new-todo').disabled,true);assert.equal(host.values.get(KEY),saved);assert.equal(host.storageWrites,0);
 host.storageError=null;t.click('retry-load');assert.equal(t.rows().length,1);assert.ok(t.dom.root.textContent.includes('stored'));assert.equal(host.storageWrites,0);t.dispose();
});


test('malformed initial hashes fall back without polling or writing storage',async()=>{
 for(const hash of ['#/%ZZ','#/?query=1','#/space here','#/'+'x'.repeat(2048)]){
  const t=await mounted(testPlatform(hash));t.flush();assert.equal(t.host.location.hash,'#/');assert.equal(t.host.storageWrites,0);assert.equal(t.app.stats.locationSubscriptions,1);assert.equal(t.app.stats.scheduledTask,false);t.dispose();
 }
});
