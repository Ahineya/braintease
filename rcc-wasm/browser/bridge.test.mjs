import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { mountBrowser } from "./bridge.mjs";
import { parsePatch, planPatch, cellView, eventRecord, writeEvents } from "./protocol.mjs";
import { testDOM } from "./test-dom.mjs";

function batch(commands, strings = []) {
  const pool = [], ids = [];
  for (const string of strings) {
    const bytes = new TextEncoder().encode(string); ids.push(pool.length); pool.push(bytes.length, ...bytes);
  }
  const memory = new WebAssembly.Memory({ initial: 2, maximum: 3 });
  const cells = 8 + commands.length * 8 + pool.length;
  const view = new DataView(memory.buffer); let p = 0;
  for (const value of [0x5244, 1, cells, commands.length, 8, 8 + commands.length * 8, pool.length, 0]) view.setUint16(p++ * 2, value, true);
  for (const command of commands) for (let i = 0; i < 8; ++i) view.setUint16(p++ * 2, command[i] ?? 0, true);
  for (const value of pool) view.setUint16(p++ * 2, value, true);
  return { memory, cells, ids, parse: () => parsePatch(memory, 0, cells) };
}
function initial() {
  return { nodes: new Map([[0, { id: 0, kind: "root", parent: null, children: new Set(), listeners: new Map() }]]), maxNode: 0, maxListener: 0 };
}
function accept(state, batch) {
  const plan = planPatch(state, batch);
  for (const [id, node] of plan.draft) { if (node) state.nodes.set(id, node); else state.nodes.delete(id); }
  state.maxNode = plan.maxNode; state.maxListener = plan.maxListener;
  return plan;
}

test("wire validation rejects malformed headers, lengths, octets and UTF-8", () => {
  const good = batch([[2, 1, 0]], ["🌍é"]);
  assert.equal(good.parse().strings.get(0), "🌍é");
  for (const index of [0, 1, 2, 4, 5, 6, 7, 13]) {
    const fixture = batch([[2, 1, 0]], ["hello"]), view = new DataView(fixture.memory.buffer);
    view.setUint16(index * 2, 65535, true);
    assert.throws(fixture.parse);
  }
  for (const value of [256, 0xff]) {
    const fixture = batch([[2, 1, 0]], ["a"]);
    new DataView(fixture.memory.buffer).setUint16(17 * 2, value, true);
    assert.throws(fixture.parse);
  }
  assert.throws(() => cellView(good.memory, 1, 2, 8));
  assert.throws(() => cellView(good.memory, 0, -1, 8));
  assert.throws(() => cellView(good.memory, 0, 9, 8));
  assert.throws(() => cellView(good.memory, good.memory.buffer.byteLength - 2, 2, 8));
  // The adapter never retains an invalidated memory view.
  good.memory.grow(1);
  assert.equal(good.parse().strings.get(0), "🌍é");
});

test("preflight validates the whole batch without touching mounted state", () => {
  const state = initial();
  const valid = batch([[1, 1, 0], [6, 0, 1]], ["div"]).parse();
  const before = state.nodes.get(0);
  assert.throws(() => planPatch(state, { ...valid, commands: [...valid.commands, [3, 99, 0, 0, 0]] }));
  assert.equal(state.nodes.size, 1); assert.equal(state.nodes.get(0), before); assert.equal(before.children.size, 0);
  accept(state, valid);
  for (const commands of [
    [[1, 1, 0]], [[6, 1, 1]], [[6, 1, 0]], [[6, 0, 1, 99]],
    [[7, 0]], [[3, 1, 0]], [[5, 1, 99]], [[8, 1, 99, 1]], [[8, 1, 1, 1, 4]], [[99]],
  ]) assert.throws(() => planPatch(state, { ...valid, commands: commands.map(c => [...c, ...Array(5 - c.length).fill(0)]) }));
  for (const strings of [["script"], ["my-custom-element"]]) assert.throws(() => planPatch(initial(), batch([[1, 1, 0]], strings).parse()));
  assert.throws(() => planPatch(state, batch([[4, 1, 0, 8]], ["onclick", "alert(1)"]).parse()));
});

test("keyed moves preserve identity; removed nodes and replaced listeners retire", () => {
  const state = initial(), fixture = batch([[1, 1, 0], [1, 2, 0], [6, 0, 1], [6, 0, 2], [8, 1, 1, 1]], ["button"]).parse();
  accept(state, fixture); const first = state.nodes.get(1);
  const plan = accept(state, { ...fixture, commands: [[6, 0, 2, 1, 0], [8, 1, 1, 2, 0]] });
  assert.equal(plan.actions[0].before.id, first.id);
  assert.equal(state.nodes.get(1).listeners.get(1).id, 2);
  assert.throws(() => planPatch(state, { ...fixture, commands: [[8, 2, 1, 1, 0]] }));
  accept(state, { ...fixture, commands: [[7, 1, 0, 0, 0]] });
  assert.throws(() => planPatch(state, { ...fixture, commands: [[1, 1, 0, 0, 0]] }));
});

test("event ABI preserves Unicode, ordering, modifiers and input snapshots", () => {
  const memory = new WebAssembly.Memory({ initial: 1 });
  const binding = { id: 42, node: 7, kind: 2 };
  const one = eventRecord(binding, { key: "é", ctrlKey: true, isComposing: true }, { value: "A🌍", checked: true });
  const two = eventRecord({ ...binding, kind: 1 }, {}, {});
  const cells = writeEvents(memory, 0, [one, two]), v = new DataView(memory.buffer);
  assert.deepEqual(Array.from({ length: 12 }, (_, i) => v.getUint16(i * 2, true)), [0x5245, 1, cells, 2, one.size, 42, 7, 2, 18, 2, 5, 1]);
  assert.equal(v.getUint16((4 + one.size + 3) * 2, true), 1);
  assert.throws(() => eventRecord(binding, {}, { value: "a".repeat(4096) }));
});

const wasm = readFileSync(process.env.RIPPLE_TEST_WASM); // Rust harness compiles the real C fixture.
test("compiled counter batches clicks, renders once per drain and disposes completely", async () => {
  const dom = testDOM(), output = [], errors = [];
  const app = await mountBrowser(wasm, { ...dom, onOutput: text => output.push(text), onError: error => errors.push(error) });
  assert.equal(dom.root.textContent, "Hello from Wasm0000");
  assert.equal(app.stats.commits, 1); assert.equal(app.stats.commands, 9);
  const button = dom.root.childNodes[0].childNodes[1], textNode = button.childNodes[0];
  const calls = app.stats.hostCalls, entries = app.stats.guestEntries;
  assert.equal(button.dispatch("click").defaultPrevented, true);
  button.dispatch("click"); button.dispatch("click");
  assert.equal(app.stats.hostCalls, calls); assert.equal(app.stats.guestEntries, entries); // enqueue only
  assert.equal(dom.frames.size, 1); dom.frame();
  assert.equal(textNode.data, "0003"); assert.equal(button.childNodes[0], textNode);
  assert.equal(app.stats.commits, 2); assert.equal(app.stats.eventBatches, 1); assert.equal(app.stats.eventsDelivered, 3);
  assert.equal(app.stats.guestEntries, entries + 1); // one entry, including final output flush
  const snapshot = app.stats; app.drain(); assert.deepEqual(app.stats, snapshot); // unchanged view/empty drain
  // One bounded event batch per animation frame, even with queued work left.
  for (let i = 0; i < 130; ++i) button.dispatch("click");
  dom.frame(); assert.equal(app.stats.eventsDelivered, 67); assert.equal(dom.frames.size, 1);
  dom.frame(); assert.equal(app.stats.eventsDelivered, 131);
  dom.frame(); assert.equal(app.stats.eventsDelivered, 133); assert.equal(textNode.data, "0133");
  button.dispatch("click"); assert.equal(dom.frames.size, 1);
  app.dispose(); app.dispose(); dom.frame();
  assert.equal(app.stats.liveNodes, 0); assert.equal(app.stats.liveListeners, 0); assert.equal(app.stats.queuedEvents, 0);
  assert.equal(dom.root.childNodes.length, 0); assert.equal(button.listeners.get("click").size, 0);
  assert.equal(app.stats.liveAllocations, 0); assert.equal(app.stats.livePayloadCells, 0);
  assert.equal(output.join(""), "Counter ready\nCounter disposed\n"); assert.deepEqual(errors, []);
});

test("queue overflow fails explicitly, cancels work and releases DOM/listeners", async () => {
  const dom = testDOM(), errors = [];
  const app = await mountBrowser(wasm, { ...dom, onError: error => errors.push(error) });
  const button = dom.root.childNodes[0].childNodes[1];
  for (let i = 0; i < 1025; ++i) button.dispatch("click");
  assert.equal(errors.length, 1); assert.match(errors[0].message, /overflow/);
  assert.equal(app.stats.disposed, true); assert.equal(app.stats.liveNodes, 0); assert.equal(app.stats.liveListeners, 0);
  assert.equal(dom.frames.size, 0); assert.equal(dom.root.childNodes.length, 0);
});

test("output cannot synchronously reenter a guest turn", async () => {
  const dom = testDOM(); let app;
  app = await mountBrowser(wasm, { ...dom, onOutput(text) {
    if (text.includes("disposed")) assert.throws(() => app.dispose(), /active guest/);
  } });
  app.dispose(); assert.equal(app.stats.liveNodes, 0);
});

const stressWasm = readFileSync(process.env.RIPPLE_STRESS_WASM);
test("500-node render uses one commit; replacement/removal filters stale queued bindings", async () => {
  const dom = testDOM(), errors = [], output = [];
  const app = await mountBrowser(stressWasm, { ...dom, onOutput: text => output.push(text), onError: e => errors.push(e) });
  const container = dom.root.childNodes[0], button = container.childNodes[0], input = container.childNodes[1];
  const first = container.childNodes[2], last = container.childNodes.at(-1);
  assert.equal(app.stats.liveNodes, 504); assert.equal(app.stats.commits, 1); assert.equal(app.stats.commands, 1013);
  assert.equal(app.stats.liveListeners, 3); assert.equal(app.stats.liveAllocations, 1); assert.equal(app.stats.livePayloadCells, 128);
  assert.equal(output.join(""), 'a'.repeat(8191) + '🌍\n'); assert.equal(app.stats.outputCalls, 2);
  const retiredCallback = [...button.listeners.get("click")][0];
  const event = button.dispatch("click"); assert.equal(event.defaultPrevented, true); assert.equal(event.stopped, true);
  for (let i = 0; i < 130; ++i) { input.value = `value${i}`; input.dispatch("input"); }
  dom.frame();
  assert.equal(app.stats.eventsDelivered, 64); assert.equal(app.stats.commits, 2); assert.equal(app.stats.liveNodes, 503);
  assert.equal(output.at(-1), "!"); // partial output flush shares the event entry
  assert.equal(app.stats.liveListeners, 2); assert.equal(input.listeners.get("input").size, 0);
  assert.equal(button.textContent, "changed"); assert.equal(button.attributes.has("title"), false);
  assert.equal(container.childNodes[1], first); assert.equal(container.childNodes[2], last);
  dom.frame(); assert.equal(app.stats.staleEvents, 67); assert.equal(app.stats.eventBatches, 1);
  retiredCallback({preventDefault(){},stopPropagation(){}}); dom.frame(); assert.equal(app.stats.staleEvents, 68);
  button.dispatch("click");dom.frame();
  assert.equal(output.at(-1), "?");
  assert.equal(app.stats.liveNodes, 0); assert.equal(app.stats.liveListeners, 0); assert.equal(dom.root.childNodes.length, 0);
  app.dispose(); assert.equal(app.stats.liveAllocations, 0); assert.equal(app.stats.livePayloadCells, 0);
  assert.equal(output.at(-1), "D");
  assert.deepEqual(errors, []);
});

test("DOM exceptions clean up already-created nodes and listeners", async () => {
  const dom = testDOM(), create = dom.document.createElement;
  dom.document.createElement = tag => { if (tag === "input") throw Error("test DOM failure"); return create(tag); };
  await assert.rejects(() => mountBrowser(stressWasm, { ...dom }), /test DOM failure/);
  assert.equal(dom.root.childNodes.length, 0); assert.equal(dom.frames.size, 0);
});

test("oversized input is an explicit failure with complete host cleanup", async () => {
  const dom = testDOM(), errors = [];
  const app = await mountBrowser(stressWasm, { ...dom, onError: e => errors.push(e) });
  const input = dom.root.childNodes[0].childNodes[1];
  input.value = 'a'.repeat(4096); input.dispatch("input");
  assert.equal(errors.length, 1); assert.match(errors[0].message, /inbox capacity/);
  assert.equal(app.stats.liveNodes, 0); assert.equal(app.stats.liveListeners, 0); assert.equal(app.stats.disposed, true);
});

test("typed public dispatcher rejects corrupt inboxes before guest application code", async () => {
  const invalid = [
    { words: [0x5245,1,12,1,0,1,3,1,0,0,0,0], cells: 12 }, // zero-sized record
    { words: [0x5245,1,12,1,8,1,3,1,32,0,0,0], cells: 12 }, // reserved modifiers
    { words: [0x5245,1,12,1,8,1,3,11,0,0,0,0], cells: 12 }, // unknown event kind
    { words: [0x5245,1,12,2,8,1,3,1,0,0,0,0], cells: 12 }, // missing record
    { words: [0x5245,1,13,1,9,1,3,1,0,1,0,0,256], cells: 13 }, // invalid octet
    { words: [0x5245,2,4,0], cells: 4 },
    { words: [0x5245,1,4,0], cells: -1 },
  ];
  for (const { words, cells } of invalid) {
    const {instance}=await WebAssembly.instantiate(wasm,{ripple:{write(){},dom_commit(){}}});
    assert.equal(instance.exports.__ripple_start(),0);
    const address=instance.exports.__ripple_event_buffer();assert.equal(typeof address,'number');
    const view=new DataView(instance.exports.memory.buffer);
    words.forEach((word,i)=>view.setUint16(address+i*2,word,true));
    assert.throws(()=>instance.exports.__ripple_events(cells),WebAssembly.RuntimeError);
    assert.equal(instance.exports.__ripple_fault.value,8);
  }
});


test("hash hrefs and deferred focus are validated before any DOM mutation", () => {
  for (const href of ["javascript:alert(1)", "https://example.com", "#/bad path", "#/%20"]) {
    const f = batch([[1,1,0],[4,1,2,7]], ["a","href",href]);
    const [tag,name,value] = f.ids;
    f.parse = () => parsePatch(f.memory,0,f.cells);
    const parsed=f.parse();parsed.commands[1]=[4,1,name,value,0];
    assert.throws(()=>planPatch(initial(),parsed),/hash link/);
  }
  const f=batch([[1,1,0],[11,1]], ["input"]);
  assert.equal(planPatch(initial(),f.parse()).actions.at(-1).op,"focus");
  assert.throws(()=>planPatch(initial(),batch([[1,1,0],[11,1]],["div"]).parse()),/focus target/);
});
