import { ABI, EVENT_CELLS, EVENT_NAMES, cellView, octets, parsePatch, planPatch, eventRecord, writeEvents } from "./protocol.mjs";
import { taskPump } from "./task-pump.mjs";
import { browserEffects } from "./effects.mjs";
import { faults } from "../faults.mjs";

// Static browser ES module. No guest model, diff, allocator or scheduler lives
// here: this adapter applies patches and transports bounded event batches.
export async function mountBrowser(source, { root, onOutput = () => {}, onError = error => console.error(error), scheduler, effectHost = globalThis } = {}) {
  if (!root?.ownerDocument || root.childNodes.length) throw Error("browser mount requires an empty DOM root");
  const document = root.ownerDocument, window = document.defaultView;
  scheduler ??= { request: callback => window.requestAnimationFrame(callback), cancel: id => window.cancelAnimationFrame(id) };
  const state = { nodes: new Map([[0, { id: 0, kind: "root", dom: root, parent: null, children: new Set(), listeners: new Map() }]]), maxNode: 0, maxListener: 0 };
  const owned = new Set(), listeners = new Set(), pendingFocus = new Set();
  let instance, phase = "starting", inGuest = false, committing = false, scheduled = null, inboxAddress, heapAddress, heapReady = false;
  let queue = [], queueCells = 0, failure = null;
  let effectAddress, executor = false;
  const pump = taskPump({
    host: effectHost, active: () => phase === "running", pending: () => effects.pending,
    deliver: () => effects.deliver(effectAddress, enter), turn: () => enter("__ripple_turn"),
    onError: error => onError(fail(error)),
  });
  const effects = browserEffects({
    memory: () => instance.exports.memory, host: effectHost,
    notify: () => pump.request(), fatal: error => onError(fail(error)),
    schedule: delay => pump.hint(delay), supportsSchedule: () => executor,
  });
  let finalHeap = { allocations: 0, liveAllocations: 0, livePayloadCells: 0 };
  const outputDecoder = new TextDecoder("utf-8", { fatal: true });
  const counts = { hostCalls: 0, commits: 0, commands: 0, patchCells: 0, decodedOctets: 0, outputCalls: 0,
    eventBatches: 0, eventsDelivered: 0, staleEvents: 0, eventCellsCopied: 0, guestEntries: 0, maxBatchCells: 0, maxQueuedEvents: 0 };
  const enter = (name, ...args) => {
    if (inGuest || committing) throw Error("synchronous browser guest reentry is forbidden");
    inGuest = true; ++counts.guestEntries;
    try {
      if (executor && ["__ripple_start", "__ripple_events", "__ripple_completions", "__ripple_turn"].includes(name)) args.push(pump.sample());
      const result = instance.exports[name](...args);
      if (failure) throw failure;
      return result;
    } catch (error) {
      const code = instance?.exports.__ripple_fault?.value ?? 0;
      if (code) throw Error(`Wasm runtime fault ${code}: ${faults[code] ?? "unknown fault"}`, { cause: error });
      throw error;
    } finally { inGuest = false; }
  };
  const unlisten = (node, binding) => {
    node.dom.removeEventListener(EVENT_NAMES[binding.kind], binding.callback);
    listeners.delete(binding);
  };
  const cleanup = () => {
    effects.dispose();
    pump.dispose();
    if (instance && heapReady) finalHeap = heapStats();
    if (scheduled !== null) scheduler.cancel(scheduled);
    scheduled = null;
    queue = []; queueCells = 0;
    for (const binding of listeners) binding.dom.removeEventListener(EVENT_NAMES[binding.kind], binding.callback);
    listeners.clear();
    for (const node of owned) node.remove();
    pendingFocus.clear(); owned.clear(); state.nodes.clear(); phase = "disposed";
    // A retained diagnostic handle must not pin the entire C16 memory image.
    instance = null;
  };
  const heapStats = () => {
    if (!instance) return finalHeap;
    const heap = cellView(instance.exports.memory, heapAddress, 6, 6);
    return { allocations: heap.getUint32(0, true), liveAllocations: heap.getUint32(4, true), livePayloadCells: heap.getUint32(8, true) };
  };
  const fail = error => { failure ??= error; cleanup(); return error; };
  const schedule = () => {
    if (scheduled !== null || phase !== "running") return;
    scheduled = scheduler.request(() => {
      scheduled = null;
      try { drain(); } catch (error) { onError(error); }
    });
  };
  function drainEffects() {
    if (phase !== "running") return;
    if (inGuest || committing) throw Error("synchronous browser guest reentry is forbidden");
    try { pump.drain(); } catch (error) { throw fail(error); }
  }
  const effectCommit = (address, cells) => {
    ++counts.hostCalls;
    if (!inGuest || committing || !["starting", "running"].includes(phase)) throw Error("effects outside an active guest turn");
    effects.commit(address, cells);
  };
  const enqueue = (binding, event, dom) => {
    if (phase !== "running" && phase !== "starting") return;
    try {
      if (binding.flags & 1) event.preventDefault();
      if (binding.flags & 2) event.stopPropagation();
      const record = eventRecord(binding, event, dom);
      if (queue.length >= 1024 || queueCells + record.size > 524288) throw Error("browser event queue overflow");
      queue.push(record); queueCells += record.size;
      counts.maxQueuedEvents = Math.max(counts.maxQueuedEvents, queue.length);
      // This callback only queues. Even events fired during a commit cannot
      // reenter a guest borrow or dispatch into a half-updated node map.
      schedule();
    } catch (error) { onError(fail(error)); }
  };
  const commit = (address, cells) => {
    ++counts.hostCalls;
    if (!inGuest || committing || !["starting", "running"].includes(phase)) throw Error("DOM commit outside an active guest turn");
    committing = true;
    try {
      const batch = parsePatch(instance.exports.memory, address, cells);
      const plan = planPatch(state, batch);
      for (const action of plan.actions) {
        if (failure) throw failure;
        const { node, binding } = action;
        switch (action.op) {
          case "create":
            node.dom = node.kind === "text" ? document.createTextNode(action.text) : node.namespace ?
              document.createElementNS("http://www.w3.org/2000/svg", node.tag) : document.createElement(node.tag);
            owned.add(node.dom); break;
          case "focus": pendingFocus.add(node.id); break;
          case "text": node.dom.data = action.value; break;
          case "attribute": node.dom.setAttribute(action.name, action.value); break;
          case "remove_attribute": node.dom.removeAttribute(action.name); break;
          case "property":
            // Preserve a controlled input's cursor/selection when unchanged.
            if (node.dom[action.name] !== action.value) node.dom[action.name] = action.value;
            break;
          case "insert": action.parent.dom.insertBefore(action.child.dom, action.before?.dom ?? null); break;
          case "remove":
            for (const removed of action.removed) {
              for (const listener of removed.listeners.values()) unlisten(removed, listener);
              owned.delete(removed.dom);
            }
            node.dom.remove(); break;
          case "listen":
            binding.dom = node.dom;
            binding.callback = event => enqueue(binding, event, node.dom);
            node.dom.addEventListener(EVENT_NAMES[binding.kind], binding.callback);
            listeners.add(binding); break;
          case "unlisten": unlisten(node, binding); break;
          default: throw Error("invalid planned DOM operation");
        }
      }
      if (failure) throw failure;
      for (const [id, node] of plan.draft) {
        if (node) state.nodes.set(id, node); else state.nodes.delete(id);
      }
      for (const id of pendingFocus) {
        const node = state.nodes.get(id);
        if (!node) pendingFocus.delete(id);
        else if (root.contains(node.dom)) { pendingFocus.delete(id); node.dom.focus({preventScroll:true}); node.dom.select?.(); }
      }
      state.maxNode = plan.maxNode; state.maxListener = plan.maxListener;
      ++counts.commits; counts.commands += batch.commands.length; counts.patchCells += cells;
      counts.decodedOctets += batch.decoded; counts.maxBatchCells = Math.max(counts.maxBatchCells, cells);
    } finally { committing = false; }
  };
  const write = (address, cells) => {
    ++counts.hostCalls; ++counts.outputCalls;
    if (!inGuest || phase === "disposed") throw Error("output outside an active guest turn");
    const bytes = octets(cellView(instance.exports.memory, address, cells, 8192), 0, cells);
    counts.decodedOctets += bytes.length;
    const text = outputDecoder.decode(bytes, { stream: true });
    if (text) onOutput(text);
  };
  function drain() {
    if (phase !== "running") return;
    if (inGuest || committing) throw Error("synchronous browser guest reentry is forbidden");
    if (scheduled !== null) { scheduler.cancel(scheduled); scheduled = null; }
    const batch = []; let cells = 4, consumed = 0;
    while (consumed < queue.length && batch.length < 64) {
      const record = queue[consumed], b = record.binding;
      if (state.nodes.get(b.node)?.listeners.get(b.kind) !== b) {
        ++counts.staleEvents; ++consumed; queueCells -= record.size; continue;
      }
      if (cells + record.size > EVENT_CELLS) break;
      cells += record.size; batch.push(record); ++consumed; queueCells -= record.size;
    }
    queue.splice(0, consumed);
    try {
      if (batch.length) {
        writeEvents(instance.exports.memory, inboxAddress, batch);
        enter("__ripple_events", cells);
        ++counts.eventBatches; counts.eventsDelivered += batch.length; counts.eventCellsCopied += cells;
      }
      if (queue.length) schedule();
    } catch (error) { throw fail(error); }
  }
  try {
    if (typeof source === "string" || source instanceof URL || source instanceof Response) {
      const response = source instanceof Response ? source : await fetch(source);
      if (!response.ok) throw Error(`cannot load browser Wasm: HTTP ${response.status}`);
      source = await response.arrayBuffer();
    }
    const result = await WebAssembly.instantiate(source, { ripple: { write, dom_commit: commit, effect_commit: effectCommit } });
    instance = result.instance ?? result;
    for (const name of ["__ripple_start", "__ripple_events", "__ripple_event_buffer", "__ripple_event_capacity", "__ripple_dispose", "__ripple_browser_abi", "__ripple_heap_metrics"]) {
      if (typeof instance.exports[name] !== "function") throw Error(`module lacks browser entry ${name}; call ripple_browser_mount from main`);
    }
    if (!(instance.exports.memory instanceof WebAssembly.Memory) || enter("__ripple_browser_abi") !== ABI || enter("__ripple_event_capacity") !== EVENT_CELLS) throw Error("incompatible browser ABI");
    inboxAddress = enter("__ripple_event_buffer");
    heapAddress = enter("__ripple_heap_metrics");
    cellView(instance.exports.memory, inboxAddress, EVENT_CELLS, EVENT_CELLS);
    cellView(instance.exports.memory, heapAddress, 6, 6);
    heapReady = true;
    executor = typeof instance.exports.__ripple_turn === "function";
    if (typeof instance.exports.__ripple_completions === "function") {
      effectAddress = enter("__ripple_effect_buffer");
      cellView(instance.exports.memory, effectAddress, 32768, 32768);
    }
    if (enter("__ripple_start") !== 0) throw Error("browser main returned a nonzero status");
    phase = "running";
    if (queue.length) schedule();
    pump.request();
  } catch (error) { throw fail(error); }
  return {
    drain, drainEffects,
    get stats() {
      return { ...counts, ...effects.stats, ...pump.stats, liveNodes: owned.size, liveListeners: listeners.size, queuedEvents: queue.length, disposed: phase === "disposed",
        ...heapStats() };
    },
    dispose() {
      if (phase === "disposed") return;
      if (inGuest || committing) throw Error("cannot dispose during an active guest turn");
      phase = "disposing";
      try {
        enter("__ripple_dispose");
        const tail = outputDecoder.decode(); if (tail) onOutput(tail);
      } finally { cleanup(); }
    },
  };
}
