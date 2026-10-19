import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { browserEffects } from "./effects.mjs";
import { mountBrowser } from "./bridge.mjs";
import { testDOM } from "./test-dom.mjs";

function clock() {
  let next = 1; const timers = new Map(), requests = [];
  const host = {
    setTimeout(fn, delay) { const id = next++; timers.set(id, { fn, delay }); return id; },
    clearTimeout(id) { timers.delete(id); },
    fetch(url, options) { return new Promise((resolve, reject) => requests.push({ url, options, resolve, reject })); },
  };
  return { host, timers, requests,
    fire(delay) { const found = [...timers].find(([, t]) => t.delay === delay); assert.ok(found, `missing ${delay}ms timer`); timers.delete(found[0]); found[1].fn(); },
  };
}
function command(memory, records) {
  const words = [0x5246, 1, 0, records.length, 0, 0, 0, 0];
  for (const { kind, id, arg = 0, url = "" } of records) {
    const bytes = new TextEncoder().encode(url);
    words.push(8 + bytes.length, kind, id & 65535, id >>> 16, arg & 65535, arg >>> 16, bytes.length, 0, ...bytes);
  }
  words[2] = words.length;
  const view = new DataView(memory.buffer);
  words.forEach((w, i) => view.setUint16(i * 2, w, true));
  return words.length;
}
const turns = async () => { for (let i = 0; i < 20; ++i) await Promise.resolve(); };

test("wake hints preflight atomically and share the effect batch", () => {
  const memory=new WebAssembly.Memory({initial:2}),c=clock(),hints=[];
  const effects=browserEffects({memory:()=>memory,host:c.host,notify(){},fatal:assert.fail,
    schedule:delay=>hints.push(delay),supportsSchedule:()=>true});
  let size=command(memory,[{kind:1,id:1,arg:2},{kind:2,id:2,arg:0,url:'./bad'}]);
  const view=new DataView(memory.buffer);view.setUint16(8,1,true);view.setUint16(10,25,true);
  assert.throws(()=>effects.commit(0,size));assert.deepEqual(hints,[]);assert.equal(c.timers.size,0);
  size=command(memory,[{kind:1,id:1,arg:2}]);
  view.setUint16(8,1,true);view.setUint16(12,32768,true);
  assert.throws(()=>effects.commit(0,size),/wake hint/);assert.equal(c.timers.size,0);
  view.setUint16(12,0,true);view.setUint16(10,25,true);effects.commit(0,size);
  assert.deepEqual(hints,[25]);assert.equal(c.timers.size,1);effects.dispose();
  size=command(memory,[]);view.setUint16(8,1,true);view.setUint16(10,65535,true);view.setUint16(12,65535,true);
  const legacy=browserEffects({memory:()=>memory,host:c.host,notify(){},fatal:assert.fail});
  assert.throws(()=>legacy.commit(0,size),/unsupported/);legacy.dispose();
});

test("effects preflight all commands, reject reused IDs and bound memory", () => {
  const memory = new WebAssembly.Memory({ initial: 2 }), c = clock();
  const effects = browserEffects({ memory: () => memory, host: c.host, notify() {}, fatal: assert.fail });
  let size = command(memory, [{ kind: 1, id: 1, arg: 2 }, { kind: 2, id: 2, arg: 0, url: "./bad" }]);
  assert.throws(() => effects.commit(0, size)); assert.equal(c.timers.size, 0);
  size = command(memory, [{ kind: 1, id: 1, arg: 2147483648 }]); assert.throws(() => effects.commit(0, size));
  size = command(memory, [{ kind: 1, id: 1, arg: 2 }]); effects.commit(0, size);
  assert.throws(() => effects.commit(0, size), /reused/);
  effects.dispose(); effects.dispose(); assert.equal(c.timers.size, 0); assert.equal(effects.stats.outstandingEffects, 0);
  assert.throws(() => effects.commit(0, size), /disposed/);
});

test("cancellation discards queued completions and late fetch resolution", async () => {
  const memory = new WebAssembly.Memory({ initial: 2 }), c = clock(); let notified = 0;
  const effects = browserEffects({ memory: () => memory, host: c.host, notify() { ++notified; }, fatal: assert.fail });
  effects.commit(0, command(memory, [{ kind: 1, id: 1, arg: 2 }, { kind: 2, id: 2, arg: 8, url: "./late" }]));
  c.fire(2); assert.equal(effects.pending, 1);
  effects.commit(0, command(memory, [{ kind: 3, id: 1 }, { kind: 3, id: 2 }]));
  assert.equal(effects.pending, 0); assert.equal(c.requests[0].options.signal.aborted, true);
  c.requests[0].resolve(new Response("late")); await turns();
  assert.equal(effects.pending, 0); assert.equal(notified, 1); assert.equal(effects.stats.lateCompletions, 1);
  effects.deliver(0, assert.fail); effects.dispose();
});

test("completion batches respect the C16 inbox limit and stream body limits", async () => {
  const memory = new WebAssembly.Memory({ initial: 2 }), c = clock();
  const effects = browserEffects({ memory: () => memory, host: c.host, notify() {}, fatal: assert.fail });
  effects.commit(0, command(memory, [1, 2].map(id => ({ kind: 2, id, arg: 16384, url: "./body" }))));
  for (const request of c.requests) request.resolve(new Response(new Uint8Array(16384), { status: 200 }));
  await turns(); assert.equal(effects.pending, 2);
  const batches = [];
  effects.deliver(0, (name, cells) => batches.push(cells));
  assert.equal(effects.pending, 1); effects.deliver(0, (name, cells) => batches.push(cells));
  assert.deepEqual(batches, [16396, 16396]); assert.equal(effects.stats.outstandingEffects, 0);
  effects.dispose();
});

test("disposal retires a fetch suspended inside a body read", async () => {
  const memory = new WebAssembly.Memory({ initial: 2 }), c = clock();
  let stream, cancelled = 0, notified = 0;
  const body = new ReadableStream({start(controller) {stream = controller;}, cancel() {++cancelled;}});
  const effects = browserEffects({memory: () => memory, host: c.host, notify() {++notified;}, fatal: assert.fail});
  effects.commit(0, command(memory, [{kind: 2, id: 1, arg: 8, url: './stream'}]));
  c.requests[0].resolve(new Response(body));await turns();effects.dispose();
  assert.equal(c.requests[0].options.signal.aborted,true);
  // A host that races/ignores AbortSignal must still have its body cancelled.
  stream.enqueue(new Uint8Array([1]));await turns();
  assert.equal(cancelled,1);assert.equal(notified,0);assert.equal(effects.pending,0);
  assert.equal(effects.stats.outstandingEffects,0);assert.equal(effects.stats.lateCompletions,1);
});

if (process.env.RIPPLE_EFFECT_WASM) {
  const wasm = readFileSync(process.env.RIPPLE_EFFECT_WASM);
  test("real Wasm overlaps timer/fetch, batches errors and ignores cancellation races", async () => {
    const dom = testDOM(), c = clock(), output = [], errors = [];
    const app = await mountBrowser(wasm, { ...dom, effectHost: c.host, onOutput: t => output.push(t), onError: e => errors.push(e) });
    assert.equal(app.stats.effectBatches, 1); assert.equal(app.stats.effectsStarted, 5);
    assert.equal(c.requests.length, 4); assert.equal(app.stats.outstandingEffects, 5);
    // Cancelling through a real DOM event is staged in guest code and flushed
    // with the other commands at the end of that public turn.
    dom.root.childNodes[0].dispatch("click"); dom.frame();
    const cancel = c.requests.find(r => r.url === './cancel');
    assert.equal(cancel.options.signal.aborted, true);
    cancel.resolve(new Response('too late')); await turns();
    c.requests.find(r => r.url === './ok').resolve(new Response('hélo', { status: 404 }));
    c.requests.find(r => r.url === './fail').reject(Error('network unavailable'));
    c.requests.find(r => r.url === './large').resolve(new Response('12345'));
    c.fire(10); await turns();
    assert.equal(app.stats.completionsDelivered, 0); // callbacks only enqueue
    assert.equal(c.timers.size, 1); // all four settle into one task
    c.fire(0);
    assert.equal(app.stats.completionBatches, 1); assert.equal(app.stats.completionsDelivered, 4);
    assert.equal(app.stats.effectBatches, 2); assert.equal(app.stats.effectCommands, 6); // no completion acknowledgements
    assert.equal(output.join('').split('').sort().join(''), 'EFLT');
    assert.deepEqual(errors, []);
    app.dispose(); app.dispose(); assert.equal(cancel.options.signal.aborted, true);
    assert.equal(app.stats.outstandingEffects, 0); assert.equal(app.stats.queuedCompletions, 0); assert.equal(c.timers.size, 0);
    assert.equal(app.stats.liveAllocations, 0); assert.equal(app.stats.livePayloadCells, 0);
    assert.equal(output.at(-1), 'D'); assert.equal(app.stats.lateCompletions, 1);
  });

  test("malformed completion records fault before calling the guest hook", async () => {
    for (const record of [
      [8, 1, 0, 1, 0, 0, 1, 0], // timer cannot fail
      [8, 2, 0, 2, 99, 0, 0, 0], // invalid HTTP status
      [9, 2, 0, 2, 200, 1, 0, 0, 256], // not an octet
      [8, 2, 0, 3, 200, 0, 0, 0], // unknown kind
      [25, 2, 0, 2, 200, 17, 0, 0, ...Array(17).fill(0)], // caller's 16-byte limit
    ]) {
      const { instance } = await WebAssembly.instantiate(wasm, { ripple: { write() {}, dom_commit() {}, effect_commit() {} } });
      instance.exports.__ripple_start();
      const address = instance.exports.__ripple_effect_buffer(), view = new DataView(instance.exports.memory.buffer);
      const words = [0x5247, 1, 4 + record.length, 1, ...record];
      words.forEach((w, i) => view.setUint16(address + i * 2, w, true));
      assert.throws(() => instance.exports.__ripple_completions(words.length), WebAssembly.RuntimeError);
      assert.equal(instance.exports.__ripple_fault.value, 8);
    }
  });
}

if (process.env.RIPPLE_EFFECT_CAPACITY_WASM) test("real Wasm bounds slots and never reuses retired identities", async () => {
  const dom = testDOM(), c = clock(), output = [];
  const app = await mountBrowser(readFileSync(process.env.RIPPLE_EFFECT_CAPACITY_WASM), {
    ...dom, effectHost: c.host, onOutput: t => output.push(t), onError: assert.fail,
  });
  assert.equal(output.join(''), 'Y');
  assert.equal(app.stats.effectsStarted,65);assert.equal(app.stats.effectCommands,130);
  assert.equal(app.stats.effectBatches,1);assert.equal(app.stats.outstandingEffects,0);
  assert.equal(c.timers.size,0);app.dispose();assert.equal(app.stats.liveAllocations,0);
});
