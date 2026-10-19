import { mountBrowser } from "../../browser/bridge.mjs";
const root = document.querySelector("#app");
const output = document.querySelector("#output");
const stats = document.querySelector("#stats");
const testing = new URL(location.href).searchParams.get("test") === "todo";
let effectHost = globalThis, removeTestData = () => {}, mounted = null, timer = null;

if (testing) {
  // Isolate the real-browser regression from the user's normal TodoMVC list.
  const prefix = "rfl.todomvc.test." + crypto.randomUUID() + ".";
  const storage = {
    getItem: key => localStorage.getItem(prefix + key),
    setItem: (key, value) => localStorage.setItem(prefix + key, value),
    removeItem: key => localStorage.removeItem(prefix + key),
  };
  removeTestData = () => storage.removeItem("rfl.todomvc.v1");
  effectHost = {
    setTimeout: setTimeout.bind(globalThis), clearTimeout: clearTimeout.bind(globalThis),
    fetch: fetch.bind(globalThis), location, history, localStorage: storage,
    addEventListener: addEventListener.bind(globalThis),
    removeEventListener: removeEventListener.bind(globalThis),
  };
  history.replaceState(null, "", "#/");
}

try {
  const app = await mountBrowser(new URL("./build/app.wasm", import.meta.url), {
    root, effectHost,
    onOutput: text => output.textContent += text,
    onError: error => {
      output.textContent += `ERROR: ${error.stack}\n`;
      document.body.dataset.test = "failed";
    },
  });
  mounted = app;
  const refresh = () => stats.textContent = JSON.stringify(app.stats, null, 2);
  refresh();
  timer = setInterval(refresh, 500);
  document.querySelector("#dispose").addEventListener("click", () => {
    app.dispose(); clearInterval(timer); refresh();
  });
  // A bfcache page remains owned and frozen; retain its instance for back/forward.
  window.addEventListener("pagehide", event => {
    if (!event.persisted) { app.dispose(); clearInterval(timer); }
  });
  if (testing) {
    const { checkTodoMVC } = await import("./test-browser.mjs");
    await checkTodoMVC({ app, root, effectHost });
    clearInterval(timer); refresh();
    document.body.dataset.test = "passed";
    output.textContent += "REAL TODOMVC BROWSER TEST PASSED\n";
  }
} catch (error) {
  document.body.dataset.test = "failed";
  output.textContent += `ERROR: ${error.stack}\n`;
} finally {
  if (testing) {
    mounted?.dispose();
    if (timer !== null) clearInterval(timer);
    if (mounted) stats.textContent = JSON.stringify(mounted.stats, null, 2);
    try { removeTestData(); }
    catch (error) {
      document.body.dataset.test = "failed";
      output.textContent += `ERROR cleaning up test storage: ${error.message}\n`;
    }
  }
}
