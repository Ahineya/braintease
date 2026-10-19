// Bounded browser services. Models, route matching and serialization stay in Wasm.
import { octets, validHashHref } from "./protocol.mjs";
const encoder = new TextEncoder();
const decoder = new TextDecoder("utf-8", { fatal: true });

// Version one deliberately has literal ASCII paths, no query/percent decoding.
export function validHash(hash) { return validHashHref(hash); }

export function platformCommand(kind, arg, view, offset, length) {
  if (kind === 4) {
    if (arg || length) throw Error("invalid location subscription");
    return {};
  }
  if (kind === 7) {
    if (!arg || arg > 256 || arg > length || length - arg > 16384) throw Error("invalid storage write");
    const key = decoder.decode(octets(view, offset, arg));
    const value = decoder.decode(octets(view, offset + arg, length - arg));
    if (key.includes("\0")) throw Error("invalid storage key");
    return { key, value };
  }
  const text = decoder.decode(octets(view, offset, length));
  if (kind === 5) {
    if (arg > 1 || !validHash(text)) throw Error("invalid hash navigation");
    return { text };
  }
  if (kind === 6 || kind === 8) {
    if (!length || length > 256 || text.includes("\0") || (kind === 6 ? (!arg || arg > 16384) : arg !== 0)) {
      throw Error("invalid storage request");
    }
    return { key: text };
  }
  throw Error("unsupported effect command");
}

export function browserPlatform(host) {
  const watches = new Set();
  const counts = { storageReads: 0, storageWrites: 0, navigations: 0 };
  return {
    start(job, command, settle) {
      const { kind, arg, text, key, value } = command;
      try {
        if (kind === 4) {
          if (!host.addEventListener || !host.removeEventListener || !host.location) throw Error("location unavailable");
          let previous = null;
          const changed = () => {
            try {
              const hash = host.location.hash;
              if (hash === previous) return;
              previous = hash;
              if (hash.length > 2048) { settle(job, 0, 2); return; }
              const bytes = encoder.encode(hash);
              if (bytes.length > 2048) settle(job, 0, 2);
              else settle(job, 0, 0, bytes);
            } catch { settle(job, 0, 3); }
          };
          // Publish cleanup before registration; a failed host registration must
          // not leave a retained callback in the platform's own watch index.
          job.cleanup = () => {
            watches.delete(changed);
            host.removeEventListener("hashchange", changed);
          };
          host.addEventListener("hashchange", changed);
          watches.add(changed);
          changed();
        } else if (kind === 5) {
          host.history[arg ? "replaceState" : "pushState"](null, "", text);
          ++counts.navigations;
          for (const changed of watches) changed();
          settle(job);
        } else if (kind === 6) {
          ++counts.storageReads;
          const stored = host.localStorage.getItem(key);
          if (stored === null) { settle(job); return; }
          // Bound before encoding too; UTF-8 cannot use fewer bytes than UTF-16 units.
          if (stored.length > arg) { settle(job, 0, 2); return; }
          const bytes = encoder.encode(stored);
          if (bytes.length > arg) settle(job, 0, 2);
          else settle(job, 1, 0, bytes);
        } else if (kind === 7) {
          host.localStorage.setItem(key, value);
          ++counts.storageWrites;
          settle(job);
        } else {
          host.localStorage.removeItem(key);
          settle(job);
        }
      } catch (error) {
        settle(job, 0, kind === 7 && error?.name === "QuotaExceededError" ? 4 : 3);
      }
    },
    get stats() { return { ...counts, locationSubscriptions: watches.size }; },
  };
}
