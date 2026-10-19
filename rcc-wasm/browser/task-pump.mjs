// Host event-loop transport only. Guest tasks and wake slots stay inside Wasm.
export function taskPump({ host, active, pending, deliver, turn, onError }) {
  let timer = null, scheduledFor = null, deadline = null, closed = false, last = -1;
  const counts = { taskWakeups: 0, executorTurns: 0 };
  function clock() {
    const value = host.now ? host.now() : performance.now();
    if (!Number.isFinite(value) || value < last || value < 0 || value > Number.MAX_SAFE_INTEGER)
      throw Error("browser clock must be finite and monotonic");
    last = value; return value;
  }
  function cancel() {
    if (timer !== null) host.clearTimeout(timer);
    timer = null; scheduledFor = null;
  }
  function request() {
    if (closed || !active()) return;
    const hasResults = pending();
    if (!hasResults && deadline === null) { cancel(); return; }
    const now = clock();
    const next = hasResults ? now : deadline;
    if (next === null) { cancel(); return; }
    if (timer !== null && (scheduledFor === next || (hasResults && scheduledFor <= now))) return;
    cancel(); scheduledFor = next;
    timer = host.setTimeout(() => {
      timer = null; scheduledFor = null; ++counts.taskWakeups;
      try { drain(); } catch (error) { onError(error); }
    }, Math.max(0, Math.ceil(next - now)));
  }
  function drain() {
    if (closed || !active()) return;
    cancel();
    if (pending()) deliver();
    else if (deadline !== null && clock() >= deadline) {
      deadline = null; ++counts.executorTurns; turn();
    }
    request();
  }
  return {
    sample: () => Math.floor(clock()) >>> 0,
    request, drain,
    hint(delay) {
      if (!Number.isInteger(delay) || delay < 0 || (delay > 2147483647 && delay !== 4294967295))
        throw Error("invalid browser wake hint");
      deadline = delay === 4294967295 ? null : clock() + delay;
      request();
    },
    get stats() { return { ...counts, scheduledTask: timer !== null }; },
    dispose() { closed = true; cancel(); deadline = null; },
  };
}
