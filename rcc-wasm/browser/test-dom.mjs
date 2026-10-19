// Deliberately small DOM fixture for transport/ownership tests. The separate
// browser-check.html runs the same compiled application against a real DOM.
export class TestNode {
  constructor(document, tag = null, data = null) {
    this.ownerDocument = document; this.tagName = tag; this.data = data;
    this.childNodes = []; this.parentNode = null; this.listeners = new Map();
    this.attributes = new Map(); this.value = ""; this.checked = false;
    this.valueWrites = 0;
  }
  get textContent() { return this.data ?? this.childNodes.map(node => node.textContent).join(""); }
  insertBefore(child, before) {
    if (child === before) return;
    if (before && before.parentNode !== this) throw Error("invalid anchor");
    child.remove();
    const index = before ? this.childNodes.indexOf(before) : this.childNodes.length;
    this.childNodes.splice(index, 0, child); child.parentNode = this;
  }
  remove() {
    if (this.parentNode) this.parentNode.childNodes.splice(this.parentNode.childNodes.indexOf(this), 1);
    this.parentNode = null;
  }
  contains(node) { return node === this || this.childNodes.some(child => child.contains(node)); }
  focus() { if (this.ownerDocument.activeElement === this) return; this.ownerDocument.activeElement?.blur(); this.ownerDocument.activeElement = this; this.dispatch('focus'); }
  blur() { if (this.ownerDocument.activeElement === this) this.ownerDocument.activeElement = null; this.dispatch('blur'); }
  select() { this.selectionStart = 0; this.selectionEnd = this.value.length; }
  setAttribute(name, value) { this.attributes.set(name, value); }
  removeAttribute(name) { this.attributes.delete(name); }
  addEventListener(kind, fn) {
    if (!this.listeners.has(kind)) this.listeners.set(kind, new Set());
    this.listeners.get(kind).add(fn);
  }
  removeEventListener(kind, fn) { this.listeners.get(kind)?.delete(fn); }
  dispatch(kind, fields = {}) {
    const event = { defaultPrevented: false, stopped: false, ...fields,
      preventDefault() { this.defaultPrevented = true; }, stopPropagation() { this.stopped = true; } };
    for (const fn of [...(this.listeners.get(kind) ?? [])]) fn(event);
    return event;
  }
}
export function testDOM() {
  const frames = new Map(); let next = 1;
  const scheduler = { request(fn) { const id = next++; frames.set(id, fn); return id; }, cancel(id) { frames.delete(id); } };
  const document = {
    createTextNode(text) { return new TestNode(document, null, text); },
    createElement(tag) { return new TestNode(document, tag); },
    createElementNS(ns, tag) { const node = new TestNode(document, tag); node.namespaceURI = ns; return node; },
  };
  return { document, root: new TestNode(document, "root"), scheduler, frames,
    frame() { const callbacks = [...frames.values()]; frames.clear(); for (const fn of callbacks) fn(); } };
}
