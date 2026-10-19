import { cellView, octets } from "./protocol.mjs";
const decoder = new TextDecoder("utf-8", { fatal: true });
const LIMIT = 32768;
import {platformCommand, browserPlatform} from "./platform.mjs";

// Host handles only actual browser operations. Futures, wakers and owned results
// remain in Wasm. Every callback queues; only the bridge enters the guest.
export function browserEffects({ memory, notify, fatal, schedule, supportsSchedule = () => false, host = globalThis }) {
  const jobs = new Map(), platform = browserPlatform(host);
  let closed = false, maxID = 0, queue = [];
  const counts = { effectBatches: 0, effectCommands: 0, effectsStarted: 0, effectsCancelled: 0,
    completionBatches: 0, completionsDelivered: 0, completionCellsCopied: 0, lateCompletions: 0 };
  const live = job => !closed && jobs.get(job.id) === job;
  function settle(job, status = 0, error = 0, bytes = new Uint8Array()) {
    if (!live(job)) { ++counts.lateCompletions; return; }
    if (job.done) {
      if (job.kind === 4) { job.status=status; job.error=error; job.bytes=bytes; }
      return;
    }
    job.done = true; job.status = status; job.error = error; job.bytes = bytes;
    queue.push(job); notify();
  }
  async function get(job, url, limit) {
    let reader;
    try {
      const response = await host.fetch(url, { signal: job.controller.signal, credentials: "same-origin" });
      if (!live(job)) {
        void response.body?.cancel().catch(() => {});
        ++counts.lateCompletions; return;
      }
      if (response.status < 100 || response.status > 599 || response.type === "opaque") throw Error("unreadable fetch response");
      const parts = []; let size = 0;
      if (response.body) {
        reader = response.body.getReader();
        for (;;) {
          const { value, done } = await reader.read();
          if (!live(job)) {
            void reader.cancel().catch(() => {});
            ++counts.lateCompletions; return;
          }
          if (done) break;
          if (value.byteLength > limit - size) {
            job.controller.abort(); void reader.cancel().catch(() => {});
            settle(job, 0, 2); return;
          }
          // The producer may reuse its chunks after the next read.
          if (value.byteLength) parts.push(value.slice());
          size += value.byteLength;
        }
      }
      const bytes = new Uint8Array(size); let p = 0;
      for (const part of parts) { bytes.set(part, p); p += part.length; }
      settle(job, response.status, 0, bytes);
    } catch (error) {
      if (live(job)) settle(job, 0, 1); else ++counts.lateCompletions;
    } finally { reader?.releaseLock(); }
  }
  function cancel(id) {
    const job = jobs.get(id);
    if (!job) return;
    jobs.delete(id); // Revoke identity before invoking host cancellation.
    if (job.kind === 1) host.clearTimeout(job.timer);
    else if (job.kind === 2) job.controller.abort();
    job.cleanup?.();
    queue = queue.filter(item => item !== job);
    ++counts.effectsCancelled;
  }
  return {
    commit(address, cells) {
      if (closed) throw Error("browser effects are disposed");
      const view = cellView(memory(), address, cells, LIMIT), word = i => view.getUint16(i * 2, true);
      if (cells < 8 || word(0) !== 0x5246 || word(1) !== 1 || word(2) !== cells || word(3) > 256 ||
          word(4) > 1 || (!word(4) && (word(5) || word(6))) || word(7)) throw Error("invalid effect batch header");
      const hint = word(5) + word(6) * 65536;
      if (word(4) && (!supportsSchedule() || (hint > 2147483647 && hint !== 4294967295)))
        throw Error("unsupported or invalid browser wake hint");
      const commands = [], active = new Set(jobs.keys()); let p = 8, nextID = maxID;
      for (let i = 0; i < word(3); ++i) {
        if (p + 8 > cells) throw Error("truncated effect command");
        const size = word(p), kind = word(p + 1), id = word(p + 2) + word(p + 3) * 65536;
        const arg = word(p + 4) + word(p + 5) * 65536, length = word(p + 6);
        if (size !== 8 + length || p + size > cells || !id || word(p + 7)) throw Error("invalid effect command");
        let url, extra;
        if (kind === 3) {
          if (arg || length) throw Error("invalid cancellation");
          active.delete(id);
        } else {
          if (id <= nextID || active.size === 64) throw Error("reused effect ID or full operation table");
          nextID = id; active.add(id);
          if (kind === 1) {
            if (arg > 2147483647 || length) throw Error("invalid timer duration");
          } else if (kind === 2) {
            if (!arg || arg > 16384 || !length || length > 2048) throw Error("invalid fetch limits");
            const bytes = octets(view, p + 8, length);
            if (bytes.includes(0)) throw Error("NUL in fetch URL");
            url = decoder.decode(bytes);
          } else extra = platformCommand(kind, arg, view, p + 8, length);
        }
        commands.push({ kind, id, arg, url, ...extra }); p += size;
      }
      if (p !== cells) throw Error("trailing effect cells");
      // No side effect until the complete batch passes preflight.
      for (const command of commands) {
        const { kind, id, arg, url } = command;
        if (kind === 3) { cancel(id); continue; }
        const job = { id, kind, done: false }; jobs.set(id, job);
        ++counts.effectsStarted;
        if (kind === 1) job.timer = host.setTimeout(() => settle(job), arg);
        else if (kind === 2) { job.controller = new AbortController(); void get(job, url, arg).catch(fatal); }
        else platform.start(job, command, settle);
      }
      if (word(4)) schedule(hint);
      maxID = nextID; ++counts.effectBatches; counts.effectCommands += commands.length;
    },
    deliver(address, enter) {
      if (closed || !queue.length) return;
      const batch = []; let cells = 4;
      while (queue.length && batch.length < 64 && cells + 8 + queue[0].bytes.length <= LIMIT) {
        const job = queue.shift();
        if (!live(job)) continue;
        cells += 8 + job.bytes.length; batch.push(job);
      }
      if (!batch.length) return;
      const view = cellView(memory(), address, cells, LIMIT); let p = 0;
      const put = value => view.setUint16(p++ * 2, value, true);
      for (const value of [0x5247, 1, cells, batch.length]) put(value);
      for (const job of batch) {
        for (const value of [8 + job.bytes.length, job.id & 65535, job.id >>> 16, job.kind, job.status, job.bytes.length, job.error, 0]) put(value);
        for (const byte of job.bytes) put(byte);
        if (job.kind === 4) job.done = false; else jobs.delete(job.id);
      }
      enter("__ripple_completions", cells);
      ++counts.completionBatches; counts.completionsDelivered += batch.length; counts.completionCellsCopied += cells;
      if (queue.length) notify();
    },
    get pending() { return queue.length; },
    get stats() { return { ...counts, ...platform.stats, outstandingEffects: jobs.size, queuedCompletions: queue.length }; },
    dispose() {
      if (closed) return;
      closed = true;
      for (const id of jobs.keys()) cancel(id);
      queue = [];
    },
  };
}
