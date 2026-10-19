import { mountBrowser } from "../../browser/bridge.mjs";
const output = document.querySelector("#output"), stats = document.querySelector("#stats");
try {
  const app = await mountBrowser(new URL("./build/app.wasm", import.meta.url), {
    root: document.querySelector("#app"),
    onOutput(text) { output.textContent += text; },
    onError(error) { output.textContent += `\nERROR: ${error.message}\n`; },
  });
  const refresh = () => { stats.textContent = JSON.stringify(app.stats, null, 2); };
  refresh();
  const timer = setInterval(refresh, 250);
  document.querySelector("#dispose").addEventListener("click", () => { app.dispose(); clearInterval(timer); refresh(); });
  window.addEventListener("pagehide", () => { app.dispose(); clearInterval(timer); }, { once: true });
  // Optional real-browser regression, using the actual DOM, event loop and Wasm.
  const test = new URL(location.href).searchParams.get("test");
  if (test === "ui") {
    const root=document.querySelector("#app"),counter=root.querySelector('[data-action="increment"]'),countText=counter.lastChild;
    const name=root.querySelector("input"),list=root.querySelector("ul"),rows=[...list.children];
    const wait=async check=>{const deadline=performance.now()+5000;while(!check()){if(performance.now()>deadline)throw Error("UI browser test timed out");await new Promise(resolve=>setTimeout(resolve,10));}};
    if(root.querySelector("select").value!=="Grace")throw Error("controlled select value applied before its options");
    const before=app.stats;counter.click();counter.click();counter.click();
    if(app.stats.guestEntries!==before.guestEntries)throw Error("UI event reentered Wasm");
    await wait(()=>counter.textContent==="Count: 3");
    if(app.stats.commits!==before.commits+1 || counter.lastChild!==countText)throw Error("UI counter did not batch/preserve identity");
    root.querySelector('[data-action="reverse"]').click();await wait(()=>list.firstChild===rows[2]);
    if(list.children[1]!==rows[1] || list.lastChild!==rows[0])throw Error("keyed list lost identity");
    name.focus();name.value="Ada 🌍";name.setSelectionRange(2,2);name.dispatchEvent(new Event("input",{bubbles:true}));
    await wait(()=>root.textContent.includes("Hello, Ada 🌍!"));
    if(document.activeElement!==name || name.selectionStart!==2 || name.selectionEnd!==2)throw Error("controlled input lost focus/selection");
    list.firstChild.querySelector("button").click();await wait(()=>root.textContent.includes("Selected: Edsger"));
    const unchanged=app.stats;list.firstChild.querySelector("button").click();await wait(()=>app.stats.eventBatches>unchanged.eventBatches);
    if(app.stats.hostCalls!==unchanged.hostCalls)throw Error("unchanged view crossed into JavaScript");
    root.querySelector('[data-action="later"]').click();await wait(()=>root.textContent.includes("Timer complete"));
    if(app.stats.outstandingEffects || app.stats.scheduledTask)throw Error("idle UI kept scheduling");
    const select=root.querySelector("select"),eventBatches=app.stats.eventBatches;
    select.value="Ada";select.dispatchEvent(new Event("input",{bubbles:true}));await wait(()=>app.stats.eventBatches>eventBatches);
    counter.click();await wait(()=>counter.textContent==="Count: 4");
    if(select.value!=="Ada")throw Error("controlled select did not retain its model value");
    app.dispose();clearInterval(timer);refresh();
    if(root.childNodes.length || app.stats.liveNodes || app.stats.liveListeners || app.stats.liveAllocations!==1 || app.stats.livePayloadCells!==520)throw Error("UI disposal leaked resources");
    document.body.dataset.test="passed";output.textContent+="\nREAL UI BROWSER TEST PASSED\n";
  } else if (test === "async") {
    const root=document.querySelector("#app"),paragraph=root.querySelector("p"),text=paragraph.firstChild;
    const wait=async check=>{
      const deadline=performance.now()+5000;
      while(!check()) {if(performance.now()>deadline)throw Error("async browser test timed out");await new Promise(resolve=>setTimeout(resolve,10));}
    };
    await wait(()=>paragraph.textContent.includes("Hello from a real browser fetch! 🌍") && root.textContent.includes("Clock timer complete"));
    const [reload,cancel]=root.querySelectorAll("button"),entries=app.stats.guestEntries;
    reload.click();cancel.click();
    if(app.stats.guestEntries!==entries)throw Error("event callback reentered Wasm synchronously");
    await wait(()=>paragraph.textContent==="Request cancelled.");
    reload.click();await wait(()=>paragraph.textContent.includes("Hello from a real browser fetch! 🌍"));
    if(paragraph.firstChild!==text)throw Error("async update lost node identity");
    if(app.stats.outstandingEffects || app.stats.scheduledTask)throw Error("completed application kept polling");
    app.dispose();clearInterval(timer);refresh();
    if(app.stats.liveNodes || app.stats.liveListeners || app.stats.liveAllocations!==1 || app.stats.livePayloadCells!==520 || app.stats.outstandingEffects || app.stats.scheduledTask || root.childNodes.length)throw Error("async disposal leaked resources");
    document.body.dataset.test="passed";output.textContent+="\nREAL ASYNC BROWSER TEST PASSED\n";
  } else if (test !== null) {
    const button = document.querySelector("#app button"), text = button.firstChild;
    const initial = app.stats;
    button.click(); button.click(); button.click();
    if (app.stats.eventsDelivered !== 0) throw Error("event callback reentered Wasm synchronously");
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    if (app.stats.eventBatches !== 1 || app.stats.eventsDelivered !== 3 || app.stats.commits !== initial.commits + 1 || button.firstChild !== text) throw Error("incorrect batched render or lost node identity");
    app.dispose(); clearInterval(timer); refresh();
    if (app.stats.liveNodes || app.stats.liveListeners || app.stats.liveAllocations !== initial.liveAllocations || app.stats.livePayloadCells !== initial.livePayloadCells || document.querySelector("#app").childNodes.length) throw Error("disposal leaked resources");
    document.body.dataset.test = "passed";
    output.textContent += "\nREAL BROWSER TEST PASSED\n";
  }
} catch (error) {
  document.body.dataset.test = "failed";
  output.textContent += `\nERROR: ${error.stack}\n`;
}
