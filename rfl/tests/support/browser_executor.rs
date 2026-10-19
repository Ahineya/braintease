// Included by wasm_browser.rs to share its real-Wasm/Node fixture.
use super::Fixture;

#[test]
fn generic_tail_calls_infer_unit_callback_results() {
    let source = std::fs::read_to_string(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests/fixtures/browser-callback-inference.rfl"),
    )
    .unwrap();
    let _ = Fixture::compile(&source);
}

#[test]
fn managed_callback_root_survives_main_and_releases_after_active_dispatch() {
    Fixture::compile(r#"
import {mount} from "std/browser";
import {share,with_shared} from "std/shared";
fn main(){
 mount();let value=share(owned(42,fn(n:&mut i16)=>{println("root dropped");}));
 let handler:fn(u16,u32)->u32=fn(op:u16,n:u32)=>{
   if(op==1){drop(__browser_root_replace(fn(a:u16,b:u32)=>0));}
   with_shared(value,once fn(owner:&mut Owned<i16>)=>{println(*owner.borrow());});
   0
 };
 drop(__browser_root_replace(handler));println("main returned");
}
export extern "C" fn ripple_browser_on_events(data:*const u16,cells:u32){}
export extern "C" fn ripple_browser_on_dispose(){let h=__browser_root_get();h(1,0);println("disposal callback returned");}
"#).run(r#"
assert.equal(output.join(''),'main returned\n');
app.dispose();
assert.equal(output.join(''),'main returned\n42\ndisposal callback returned\nroot dropped\n');
assert.equal(app.stats.liveAllocations,1);assert.deepEqual(errors,[]);
"#);
}

#[test]
fn mounted_async_roots_yield_use_clock_timers_and_spawn_without_reentry() {
    let source = std::fs::read_to_string(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests/fixtures/browser-executor.rfl"),
    )
    .unwrap();
    Fixture::compile(&source).run(
        r#"
assert.equal(output.join(''),'main returned\nstart\n');
assert.equal(effectTimers.size,1);assert.equal(app.stats.effectsStarted,0);
effectStep(0);assert.equal(output.join(''),'main returned\nstart\nyielded\n');
effectStep(0);assert.equal(output.join(''),'main returned\nstart\nyielded\nchild\n');
assert.equal(effectTimers.size,1);assert.equal([...effectDelays.values()][0],25);
effectStep(25);assert.equal(output.join(''),'main returned\nstart\nyielded\nchild\nclock timer\n');
assert.equal(effectTimers.size,0);assert.equal(app.stats.scheduledTask,false);
const before=app.stats;app.drainEffects();assert.deepEqual(app.stats,before);
app.dispose();assert.equal(app.stats.liveAllocations,1);assert.deepEqual(errors,[]);
"#,
    );
}

#[test]
fn mounted_fetch_events_cancellation_and_disposal_preserve_ownership_and_batching() {
    let source = std::fs::read_to_string(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests/fixtures/browser-executor-effects.rfl"),
    )
    .unwrap();
    Fixture::compile(&source).run(r#"
assert.equal(app.stats.outstandingEffects,2);assert.equal(app.stats.effectBatches,1);
for(let i=0;i<20;++i)await Promise.resolve();effectStep(0);
assert.equal(output.join(''),'200\n[104, 195, 169]\n');
assert.equal(app.stats.effectBatches,1); // successful result release stays in Wasm
const run=dom.root.childNodes[0],cancel=dom.root.childNodes[1];
cancel.dispatch('click');assert.equal(app.stats.effectsCancelled,0);dom.frame();
assert.equal(output.at(-2),'true\n');assert.equal(output.at(-1),'timer cleanup\n');
assert.equal(app.stats.outstandingEffects,0);assert.equal(app.stats.effectsCancelled,1);
cancel.dispatch('click');dom.frame();assert.equal(output.at(-1),'false\n');
let baseline=null;
for(let cycle=0;cycle<20;++cycle){
 const before=app.stats;
 run.dispatch('click');run.dispatch('click');run.dispatch('click');dom.frame();
 assert.equal(app.stats.effectBatches,before.effectBatches+1);
 for(let i=0;i<30;++i)await Promise.resolve();effectStep(0);
 assert.equal(app.stats.completionsDelivered,before.completionsDelivered+3);
 assert.equal(app.stats.completionBatches,before.completionBatches+1);
 if(baseline){assert.equal(app.stats.liveAllocations,baseline.liveAllocations);assert.equal(app.stats.livePayloadCells,baseline.livePayloadCells);}
 else baseline=app.stats;
 assert.equal(effectTimers.size,0);
}
// Dispose with an actual request in flight, before its Promise has settled.
run.dispatch('click');dom.frame();const request=effectRequests.at(-1);
app.dispose();const entries=app.stats.guestEntries;
assert.equal(request.options.signal.aborted,true);
for(let i=0;i<30;++i)await Promise.resolve();
assert.equal(app.stats.guestEntries,entries);assert.equal(output.at(-1),'app dropped\n');
assert.equal(app.stats.outstandingEffects,0);assert.equal(app.stats.queuedCompletions,0);
assert.equal(app.stats.liveAllocations,1);assert.equal(app.stats.liveNodes,0);assert.equal(app.stats.liveListeners,0);
assert.equal(effectTimers.size,0);assert.deepEqual(errors,[]);
"#);
}

#[test]
fn root_capacity_queued_cancellation_and_reentrant_cleanup_are_safe() {
    let source = std::fs::read_to_string(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests/fixtures/browser-executor-capacity.rfl"),
    )
    .unwrap();
    Fixture::compile(&source).run(r#"
assert.equal(output.join(''),'cleanup full\nfull\nqueued dropped\n');
assert.equal(effectTimers.size,0);
dom.root.childNodes[0].dispatch('click');dom.frame();assert.equal(output.at(-1),'true\n');
assert.equal(effectTimers.size,1);assert.equal([...effectDelays.values()][0],100);
app.dispose();assert.equal(output.at(-1),'cleanup closed\n');
assert.equal(effectTimers.size,0);assert.equal(app.stats.liveAllocations,1);assert.deepEqual(errors,[]);
"#);
}

#[test]
fn ordinary_async_browser_demo_runs_without_application_ffi_hooks() {
    let source = std::fs::read_to_string(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/browser-async.rfl"),
    )
    .unwrap();
    Fixture::compile(&source).run(
        r#"
const paragraph=dom.root.childNodes[0],text=paragraph.childNodes[0];
assert.equal(text.data,'Fetching a message...');
for(let i=0;i<20;++i)await Promise.resolve();effectStep(0);
assert.equal(text.data,'hé');assert.equal(paragraph.childNodes[0],text);
effectStep(750);assert.ok(dom.root.textContent.includes('Clock timer complete'));
assert.equal(app.stats.outstandingEffects,0);assert.equal(effectTimers.size,0);
app.dispose();assert.equal(app.stats.liveAllocations,1);assert.deepEqual(errors,[]);
"#,
    );
}
