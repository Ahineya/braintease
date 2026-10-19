use std::{
    fs,
    path::PathBuf,
    process::Command,
    sync::atomic::{AtomicU64, Ordering},
};

struct Fixture(PathBuf);
impl Fixture {
    fn compile(source: &str) -> Self {
        Self::compile_with_c(source, None)
    }
    fn compile_with_c(source: &str, helper: Option<&str>) -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = std::env::temp_dir().join(format!(
            "rfl-browser-{}-{stamp}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        // Wall-clock resolution alone is insufficient for parallel tests.
        fs::create_dir(&root).unwrap();
        let fixture = Self(root);
        let input = fixture.0.join("app.rfl");
        fs::write(&input, source).unwrap();
        if let Some(helper) = helper {
            fs::write(fixture.0.join("fixture.c"), helper).unwrap();
        }
        rfl::compile_file_to_wasm(&input, &fixture.0.join("app.wasm")).unwrap();
        fixture
    }
    fn compile_path(input: &std::path::Path) -> Self {
        let root = std::env::temp_dir().join(format!(
            "rfl-browser-path-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir(&root).unwrap();
        let fixture = Self(root);
        rfl::compile_file_to_wasm(input, &fixture.0.join("app.wasm")).unwrap();
        fixture
    }
    fn run(&self, body: &str) {
        let header = r#"
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {pathToFileURL} from 'node:url';
const {mountBrowser}=await import(pathToFileURL(process.argv[2]));
const {testDOM}=await import(pathToFileURL(process.argv[3]));
const dom=testDOM(), output=[], errors=[];
const effectRequests=[],effectTimers=new Map(),effectDelays=new Map();let nextEffectTimer=1,effectTime=0;
function effectStep(delay){const found=[...effectDelays].find(([id,ms])=>effectTimers.has(id)&&ms===delay);assert.ok(found,`missing ${delay}ms timer`);effectTime+=delay;const id=found[0],fn=effectTimers.get(id);effectTimers.delete(id);effectDelays.delete(id);fn();}
const effectHost={
 now:()=>effectTime,
 setTimeout(fn,ms){const id=nextEffectTimer++;effectTimers.set(id,fn);effectDelays.set(id,ms);return id;},
 clearTimeout(id){effectTimers.delete(id);effectDelays.delete(id);},
 fetch:async(url,options)=>{effectRequests.push({url,options});return new Response(new Uint8Array([104,195,169]),{status:200});}
};
const app=await mountBrowser(readFileSync(process.argv[1]),{...dom,effectHost,onOutput:t=>output.push(t),onError:e=>errors.push(e)});
"#;
        let browser = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../rcc-wasm/browser");
        let output = Command::new(std::env::var_os("NODE").unwrap_or_else(|| "node".into()))
            .args(["--input-type=module", "-e", &format!("{header}\n{body}")])
            .arg(self.0.join("app.wasm"))
            .arg(browser.join("bridge.mjs"))
            .arg(browser.join("test-dom.mjs"))
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "{}\n{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

#[test]
fn rfl_browser_example_has_batched_click_delivery_unicode_and_no_owned_leaks() {
    Fixture::compile(include_str!("../examples/browser.rfl")).run(r#"
assert.equal(dom.root.textContent,'Hello, browser 🌍 — click me!');
assert.equal(app.stats.commits,1);assert.equal(app.stats.liveNodes,2);assert.equal(app.stats.liveListeners,1);
const baseline=app.stats;
const button=dom.root.childNodes[0],text=button.childNodes[0];
button.dispatch('click');button.dispatch('click');
assert.equal(app.stats.eventsDelivered,0);dom.frame();
assert.equal(app.stats.eventsDelivered,2);assert.equal(app.stats.eventBatches,1);assert.equal(app.stats.commits,2);
assert.equal(button.childNodes[0],text);assert.equal(text.data,'Wasm received your click 🌍');
for(let i=0;i<20;++i){button.dispatch('click');dom.frame();assert.equal(app.stats.liveAllocations,baseline.liveAllocations);assert.equal(app.stats.livePayloadCells,baseline.livePayloadCells);}
app.dispose();app.dispose();
assert.equal(app.stats.liveNodes,0);assert.equal(app.stats.liveListeners,0);assert.equal(app.stats.liveAllocations,baseline.liveAllocations);assert.equal(app.stats.livePayloadCells,baseline.livePayloadCells);
assert.equal(output.join(''),'RFL browser ready 🌍\n'+'Click received in RFL!\n'.repeat(22)+'RFL browser disposed\n');
assert.deepEqual(errors,[]);
"#);
}

#[test]
fn rfl_copies_event_payloads_and_aborts_or_skips_empty_batches() {
    Fixture::compile(r#"
import {mount,patch,decode_events,INPUT,KEYDOWN} from "std/browser";
fn main(){
    mount();
    {let mut abandoned=patch();let name=abandoned.string("unused");abandoned.text(1,name);}
    let empty=patch();move(empty).commit();
    let mut dom=patch();let tag=dom.string("input");
    dom.element(1,tag);dom.insert(0,1,0);dom.listen(1,INPUT,1,0);dom.listen(1,KEYDOWN,2,0);move(dom).commit();
}
export extern "C" fn ripple_browser_on_events(data:*const u16,cells:u32){
    let events=decode_events(data,cells).unwrap();
    for(event in events){println(event.listener);println(event.key);println(event.value);println(event.modifiers);println(event.checked);}
    let mut malformed:[u16;4]=[21061,1,5,0];
    let parsed=unsafe{decode_events(&malformed[0],4)};
    match parsed{Err(_)=>println("rejected"),Ok(_)=>println("BAD")};
}
export extern "C" fn ripple_browser_on_dispose(){}
"#).run(r#"
assert.equal(app.stats.commits,1);assert.equal(app.stats.commands,4);
const baseline=app.stats;
const input=dom.root.childNodes[0];input.value='A🌍é';input.checked=true;
input.dispatch('input',{isComposing:true});input.value='later';input.dispatch('keydown',{key:'é',ctrlKey:true});
dom.frame();
assert.equal(output.join(''),'1\n\nA🌍é\n16\ntrue\n2\né\n\n2\ntrue\nrejected\n');
assert.equal(app.stats.eventsDelivered,2);assert.deepEqual(errors,[]);
app.dispose();assert.equal(app.stats.liveAllocations,baseline.liveAllocations);assert.equal(app.stats.livePayloadCells,baseline.livePayloadCells);
"#);
}

#[test]
fn browser_futures_are_lazy_owned_and_decode_owned_responses() {
    Fixture::compile_with_c(r#"
import {} from "./fixture.c";
import {mount} from "std/browser";
import {browser_effects,BrowserEffects,EffectWaiter,BrowserOperation,TimerRequest,GetRequest,sleep,get,InvalidDuration,InvalidRequest} from "std/browser_effects";
import {Future,Pending,Ready} from "std/future";
import {Context,task_set} from "std/task";
import {share} from "std/shared";
extern "C" {fn remember(slot:u16,id:u32);fn remembered(slot:u16)->u32;}
fn registry()->BrowserEffects {let xs:Vec<EffectWaiter> = [];BrowserEffects{waiting:share(move(xs))}}
fn main(){
    mount();let effects=browser_effects();
    match sleep(effects,2147483648) {Err(InvalidDuration)=>println("duration rejected"),_=>println("BAD")};
    match get(effects,"",16) {Err(InvalidRequest)=>println("url rejected"),_=>println("BAD")};
    {let lazy=sleep(effects,100).unwrap();drop(move(lazy));}
    let queue=task_set(1,0).unwrap();let waker=queue.allocate().unwrap();waker.take_ready();
    let mut cx=Context{waker:waker,now:0};
    {let mut cancel=sleep(effects,100).unwrap();
     match cancel.poll(&mut cx){Pending=>println("pending"),_=>println("BAD")};
     drop(move(cancel));effects.wake_completed();
     println(waker.take_ready());}
    // Exercise pending/ready halves through the C registry without teaching
    // C about RFL frames. The mounted executor will own these frames in W4b.
    let mut timer=BrowserOperation{effects:effects,request:Some(TimerRequest(5)),id:0,done:false};
    match timer.poll(&mut cx){Pending=>{},_=>println("BAD")};remember(0,timer.id);
    let mut request=BrowserOperation{effects:effects,request:Some(GetRequest("./body",16)),id:0,done:false};
    match request.poll(&mut cx){Pending=>{},_=>println("BAD")};remember(1,request.id);
}
export extern "C" fn ripple_browser_on_events(data:*const u16,cells:u32){}
export extern "C" fn ripple_browser_on_effects(){
    let queue=task_set(1,0).unwrap();let waker=queue.allocate().unwrap();
    let mut cx=Context{waker:waker,now:5};
    for(i in 0..2){
        let mut operation=BrowserOperation{effects:registry(),request:None,id:remembered(i as u16),done:false};
        match operation.poll(&mut cx){
            Ready(Ok(response))=>{println(response.status);println(response.body);},
            Ready(Err(_))=>println("BAD"),Pending=>println("BAD"),
        }
        operation.cancel();
    }
}
export extern "C" fn ripple_browser_on_dispose(){println("disposed");}
"#, Some("static unsigned long ids[2];void remember(unsigned slot,unsigned long id){ids[slot]=id;}unsigned long remembered(unsigned slot){return ids[slot];}")).run(r#"
assert.equal(output.join(''),'duration rejected\nurl rejected\npending\nfalse\n');
// A never-polled timer produced no command; the polled-and-dropped timer
// produced start+cancel in one batch. Only two actual operations remain.
assert.equal(app.stats.effectBatches,1);assert.equal(app.stats.effectCommands,4);
assert.equal(app.stats.outstandingEffects,2);
for(let i=0;i<20;++i)await Promise.resolve();
// Fire all operation timers, then the one coalesced completion task.
const callbacks=[...effectTimers.values()];effectTimers.clear();for(const fn of callbacks)fn();
app.drainEffects();
assert.equal(output.join(''),'duration rejected\nurl rejected\npending\nfalse\n0\n[]\n200\n[104, 195, 169]\n');
assert.equal(app.stats.outstandingEffects,0);assert.deepEqual(errors,[]);
app.dispose();assert.equal(app.stats.queuedCompletions,0);assert.equal(output.at(-1),'disposed\n');
assert.equal(app.stats.liveAllocations,1); // only the reusable synchronous frame arena
"#);
}

#[path = "support/browser_executor.rs"]
mod executor_cases;

#[path = "support/browser_ui.rs"]
mod ui_cases;
