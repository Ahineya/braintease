// Browser wire ABI v1. All lengths/offsets below count C16 cells.
export const ABI = 1;
export const EVENT_CELLS = 4096;
export const EVENT_NAMES = [null, "click", "input", "change", "keydown", "keyup", "compositionstart", "compositionend", "dblclick", "blur", "focus"];
const htmlTags = new Set("div span p button input label ul ol li h1 h2 h3 pre code section article header footer main nav strong em br a form textarea select option img canvas table tbody tr td th".split(" "));
const svgTags = new Set("svg g path circle rect line polyline polygon text defs use".split(" "));
const attributes = new Set("id class title role alt width height tabindex viewBox d cx cy r x y x1 x2 y1 y2 fill stroke stroke-width points transform type name placeholder for href maxlength".split(" "));
const decoder = new TextDecoder("utf-8", { fatal: true });
function attributeName(name) {
  if (!attributes.has(name) && !/^(aria|data)-[a-z][a-z0-9-]*$/.test(name)) throw Error(`unsupported DOM attribute ${name}`);
  return name;
}

export function validHashHref(value) { return value.startsWith("#/") && value.length <= 2048 && !/[^A-Za-z0-9/_~.\-]/.test(value.slice(2)); }
export function cellView(memory, address, cells, limit) {
  if (!Number.isInteger(address) || address < 0 || address % 2 ||
      !Number.isInteger(cells) || cells < 0 || cells > limit ||
      address + cells * 2 > memory.buffer.byteLength) throw Error("invalid C16 buffer range");
  // A fresh view per borrowed range also works with future growing memories.
  return new DataView(memory.buffer, address, cells * 2);
}
export function octets(view, offset, count) {
  const bytes = new Uint8Array(count);
  for (let i = 0; i < count; ++i) {
    const value = view.getUint16((offset + i) * 2, true);
    if (value > 255) throw Error("C16 UTF-8 cell is not an octet");
    bytes[i] = value;
  }
  return bytes;
}
export function parsePatch(memory, address, cells) {
  const view = cellView(memory, address, cells, 32768);
  const word = i => view.getUint16(i * 2, true);
  if (cells < 8 || word(0) !== 0x5244 || word(1) !== ABI || word(2) !== cells ||
      word(3) > 1024 || word(4) !== 8 || word(5) !== 8 + word(3) * 8 ||
      word(5) + word(6) !== cells || word(7) !== 0) throw Error("invalid DOM batch header");
  const strings = new Map();
  let decoded = 0;
  for (let p = word(5); p < cells;) {
    const id = p - word(5), count = word(p++);
    if (p + count > cells) throw Error("truncated DOM string pool");
    strings.set(id, decoder.decode(octets(view, p, count)));
    decoded += count;
    p += count;
  }
  const commands = [];
  for (let p = 8; p < word(5); p += 8) {
    if (word(p + 5) || word(p + 6) || word(p + 7)) throw Error("reserved DOM command cells must be zero");
    commands.push([word(p), word(p + 1), word(p + 2), word(p + 3), word(p + 4)]);
  }
  return { commands, strings, cells, decoded };
}

// Validate every operation before the first DOM mutation. Only touched metadata
// is copied; a small update does not clone or read back the mounted DOM tree.
export function planPatch(state, batch) {
  const draft = new Map(), actions = [];
  let maxNode = state.maxNode, maxListener = state.maxListener;
  const read = id => {
    const node = draft.has(id) ? draft.get(id) : state.nodes.get(id);
    if (!node) throw Error(`unknown/retired DOM node ${id}`);
    return node;
  };
  const edit = id => {
    const old = read(id);
    if (!draft.has(id)) draft.set(id, { ...old, children: new Set(old.children), listeners: new Map(old.listeners) });
    return draft.get(id);
  };
  const string = id => {
    if (!batch.strings.has(id)) throw Error(`invalid DOM string ID ${id}`);
    return batch.strings.get(id);
  };
  const element = id => {
    const node = read(id);
    if (!id || node.kind !== "element") throw Error("operation requires an owned element");
    return node;
  };
  const unused = (...values) => { if (values.some(Boolean)) throw Error("unused DOM operands must be zero"); };
  for (const [op, a, b, c, d] of batch.commands) {
    switch (op) {
      case 1: case 2: {
        if (!a || a <= maxNode) throw Error("DOM node IDs must increase and cannot be reused");
        maxNode = a;
        let tag = null, text = null;
        if (op === 1) {
          tag = string(b);
          if (c > 1 || !(c ? svgTags : htmlTags).has(tag)) throw Error(`unsupported DOM tag ${tag}`);
          unused(d);
        } else { text = string(b); unused(c, d); }
        const node = { id: a, kind: op === 1 ? "element" : "text", namespace: c, tag,
          parent: null, children: new Set(), listeners: new Map(), dom: null };
        draft.set(a, node);
        actions.push({ op: "create", node, text });
        break;
      }
      case 3: {
        const node = read(a);
        if (node.kind !== "text") throw Error("set_text requires a text node");
        unused(c, d); actions.push({ op: "text", node, value: string(b) }); break;
      }
      case 4: {
        const node = element(a), name = attributeName(string(b)), value = string(c);
        if (name === "href" && (node.namespace || node.tag !== "a" || !validHashHref(value))) throw Error("invalid hash link");
        if (name === "type" && !new Set(["text", "checkbox", "button", "number", "email", "password", "search", "submit"]).has(value)) throw Error("unsupported input type");
        unused(d); actions.push({ op: "attribute", node, name, value }); break;
      }
      case 10: {
        unused(c, d); actions.push({ op: "remove_attribute", node: element(a), name: attributeName(string(b)) }); break;
      }
      case 5: {
        const node = element(a);
        const allowed = b === 1 ? ["input", "textarea", "select", "option"] : b === 2 ? ["input"] : b === 3 ? ["input", "button", "textarea", "select", "option"] : [];
        if (node.namespace || !allowed.includes(node.tag)) throw Error("invalid DOM property for element");
        if (b !== 1 && c > 1) throw Error("boolean DOM property requires 0 or 1");
        unused(d); actions.push({ op: "property", node, name: [null, "value", "checked", "disabled"][b], value: b === 1 ? string(c) : Boolean(c) }); break;
      }
      case 11: {
        const node = element(a); unused(b, c, d);
        if (node.namespace || !["input", "textarea", "button", "select", "a"].includes(node.tag)) throw Error("invalid focus target");
        actions.push({op:"focus", node}); break;
      }
      case 6: {
        const parent = edit(a), child = edit(b);
        if (parent.kind === "text" || !b) throw Error("invalid DOM insertion");
        if (c && read(c).parent !== a) throw Error("insertion anchor is not a child of parent");
        for (let p = a; p !== null; p = read(p).parent) {
          if (p === b) throw Error("DOM insertion would create a cycle");
        }
        if (child.parent !== null) edit(child.parent).children.delete(b);
        child.parent = a; parent.children.add(b);
        unused(d); actions.push({ op: "insert", parent, child, before: c ? read(c) : null }); break;
      }
      case 7: {
        if (!a) throw Error("cannot remove mount root");
        const node = read(a), removed = [], pending = [a];
        while (pending.length) {
          const id = pending.pop(), current = read(id);
          pending.push(...current.children); removed.push(current); draft.set(id, null);
        }
        if (node.parent !== null) edit(node.parent).children.delete(a);
        unused(b, c, d); actions.push({ op: "remove", node, removed }); break;
      }
      case 8: case 9: {
        element(a); const node = edit(a);
        if (!EVENT_NAMES[b]) throw Error("unsupported DOM event kind");
        const previous = node.listeners.get(b);
        if (previous) actions.push({ op: "unlisten", node, binding: previous });
        node.listeners.delete(b);
        if (op === 8) {
          if (!c || c <= maxListener || d > 3) throw Error("listener IDs must increase; invalid listener policy");
          maxListener = c;
          const binding = { id: c, node: a, kind: b, flags: d, callback: null };
          node.listeners.set(b, binding); actions.push({ op: "listen", node, binding });
        } else unused(c, d);
        break;
      }
      default: throw Error(`unknown DOM opcode ${op}`);
    }
  }
  return { draft, actions, maxNode, maxListener };
}

const encoder = new TextEncoder();
export function eventRecord(binding, event, node) {
  const key = encoder.encode(event.key ?? "");
  const value = encoder.encode(binding.kind === 2 || binding.kind === 3 || [6, 7, 9, 10].includes(binding.kind) ? (node.value ?? "") : "");
  const size = 8 + key.length + value.length;
  if (size + 4 > EVENT_CELLS) throw Error("browser event exceeds inbox capacity");
  return { binding, size, key, value,
    flags: (event.shiftKey ? 1 : 0) | (event.ctrlKey ? 2 : 0) | (event.altKey ? 4 : 0) | (event.metaKey ? 8 : 0) | (event.isComposing ? 16 : 0),
    checked: node.checked ? 1 : 0 };
}
export function writeEvents(memory, address, records) {
  const cells = 4 + records.reduce((sum, record) => sum + record.size, 0);
  const view = cellView(memory, address, cells, EVENT_CELLS);
  let p = 0;
  const put = value => view.setUint16(p++ * 2, value, true);
  for (const value of [0x5245, ABI, cells, records.length]) put(value);
  for (const record of records) {
    const b = record.binding;
    for (const value of [record.size, b.id, b.node, b.kind, record.flags, record.key.length, record.value.length, record.checked]) put(value);
    for (const value of record.key) put(value);
    for (const value of record.value) put(value);
  }
  return cells;
}
