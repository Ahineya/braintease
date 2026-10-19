// Real compiled Wasm tests sharing the browser transport fixture.
use super::Fixture;
fn source(path: &str) -> String {
    std::fs::read_to_string(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join(path)).unwrap()
}
#[test]
fn typed_macro_app_batches_updates_preserves_keys_and_routes_async_messages() {
    Fixture::compile(&source("examples/browser-ui.rfl")).run(r#"
const section=dom.root.childNodes[0],increment=section.childNodes[1],input=section.childNodes[2].childNodes[1],reverse=section.childNodes[4],list=section.childNodes[5],later=section.childNodes[7];
assert.ok(dom.root.textContent.includes('Count: 0'));assert.equal(app.stats.commits,1);
const before=app.stats;increment.dispatch('click');increment.dispatch('click');increment.dispatch('click');
assert.equal(app.stats.guestEntries,before.guestEntries);dom.frame();
assert.ok(dom.root.textContent.includes('Count: 3'));assert.equal(app.stats.commits,before.commits+1);assert.equal(app.stats.commands,before.commands+1);
const rows=[...list.childNodes],listeners=app.stats.liveListeners;
reverse.dispatch('click');const moves=app.stats.commands;dom.frame();assert.deepEqual(list.childNodes,[...rows].reverse());assert.equal(app.stats.commands,moves+2);
assert.equal(app.stats.liveListeners,listeners);
input.value='Ada 🌍';input.dispatch('input');dom.frame();assert.ok(dom.root.textContent.includes('Hello, Ada 🌍!'));
let baseline=null;
for(let i=0;i<20;++i){reverse.dispatch('click');dom.frame();if(i%2===1){if(baseline){assert.equal(app.stats.liveAllocations,baseline.liveAllocations);assert.ok(app.stats.livePayloadCells<=baseline.livePayloadCells+512);}baseline=app.stats;}}
// Same selected message produces an unchanged view: no import or listener churn.
list.childNodes[0].childNodes[0].dispatch('click');dom.frame();const unchanged=app.stats;
list.childNodes[0].childNodes[0].dispatch('click');dom.frame();assert.equal(app.stats.hostCalls,unchanged.hostCalls);assert.equal(app.stats.commands,unchanged.commands);
later.dispatch('click');dom.frame();effectStep(0);effectStep(30);
assert.ok(dom.root.textContent.includes('Timer complete'));assert.equal(effectTimers.size,0);
// Cancel an active UI-owned future on unmount.
later.dispatch('click');dom.frame();effectStep(0);app.dispose();
assert.equal(effectTimers.size,0);assert.equal(app.stats.liveAllocations,1);assert.equal(app.stats.livePayloadCells,520);assert.deepEqual(errors,[]);
"#);
}
#[test]
fn controlled_rejection_composition_properties_and_owned_disposal() {
    Fixture::compile(&source("tests/fixtures/browser-ui-controls.rfl")).run(r#"
const root=dom.root.childNodes[0],input=root.childNodes[0],checkbox=root.childNodes[1],toggle=root.childNodes[2];
let value=input.value,writes=0;Object.defineProperty(input,'value',{get:()=>value,set(v){value=v;++writes;}});
value='rejected';input.dispatch('input');dom.frame();assert.equal(input.value,'fixed');assert.equal(writes,1);
// A controlled property without a model change must still restore user edits.
checkbox.checked=true;checkbox.dispatch('change');dom.frame();assert.equal(checkbox.checked,false);
input.dispatch('compositionstart');value='composing 🌍';input.dispatch('input',{isComposing:true});dom.frame();
assert.equal(input.value,'composing 🌍');assert.equal(writes,1);
toggle.dispatch('click');dom.frame();assert.equal(input.value,'composing 🌍');assert.equal(writes,1);
assert.equal(root.attributes.has('title'),false);assert.equal(root.attributes.get('data-phase'),'changed');
input.dispatch('compositionend');dom.frame();assert.equal(input.value,'fixed');assert.equal(writes,2);
const noChange=app.stats;input.dispatch('input');dom.frame();assert.equal(writes,2);assert.equal(app.stats.commands,noChange.commands+1);
// Only the passive clock timer remains; cleanup sees the UI sender already closed.
assert.equal(effectTimers.size,1);app.dispose();assert.equal(output.join(''),'sender closed\nmodel dropped\n');
assert.equal(effectTimers.size,0);assert.equal(app.stats.liveAllocations,1);assert.equal(app.stats.liveNodes,0);assert.deepEqual(errors,[]);
"#);
}
#[test]
fn renderer_rejects_invalid_views_and_chunks_large_trees_without_per_node_imports() {
    Fixture::compile(&source("tests/fixtures/browser-ui-renderer.rfl")).run(r#"
assert.equal(output.join(''),'duplicate rejected\nattribute rejected\n');
assert.equal(app.stats.commits,5);assert.ok(app.stats.commands>2000);assert.ok(app.stats.maxBatchCells<=32768);
assert.equal(dom.root.childNodes.length,3);const svg=dom.root.childNodes[0];assert.equal(svg.namespaceURI,'http://www.w3.org/2000/svg');
assert.equal(svg.childNodes[0].tagName,'circle');assert.equal(dom.root.childNodes[1].data,'done');
assert.equal(dom.root.childNodes[2].textContent,'direct');assert.equal(app.stats.liveNodes,5);app.dispose();assert.equal(app.stats.liveAllocations,1);assert.deepEqual(errors,[]);
"#);
}
#[test]
fn keyed_insertion_deletion_replacement_and_handler_snapshots() {
    Fixture::compile(&source("tests/fixtures/browser-ui-reconcile.rfl")).run(r#"
const section=dom.root.childNodes[0],next=section.childNodes[0],list=section.childNodes[1];
const [a,b,c]=list.childNodes,retired=b.childNodes[0];
// Both handlers are snapshotted before Advance changes the model.
next.dispatch('click');c.childNodes[0].dispatch('click');dom.frame();assert.equal(output.join(''),'0\n');
assert.deepEqual(list.childNodes.slice(0,2),[c,a]);const d=list.childNodes[2];
c.childNodes[0].dispatch('click');dom.frame();assert.equal(output.at(-1),'1\n');
const before=app.stats;retired.dispatch('click');dom.frame();assert.equal(app.stats.guestEntries,before.guestEntries);
next.dispatch('click');dom.frame();assert.deepEqual(list.childNodes.slice(0,2),[a,c]);assert.notEqual(list.childNodes[2],d);assert.equal(list.childNodes[2].tagName,'div');
next.dispatch('click');dom.frame();assert.equal(list.childNodes.length,0);assert.equal(app.stats.liveListeners,1);
next.dispatch('click');dom.frame();assert.equal(list.childNodes.length,3);assert.ok(!list.childNodes.includes(a));
app.dispose();assert.equal(app.stats.liveAllocations,1);assert.deepEqual(errors,[]);
"#);
}
#[test]
fn public_html_macro_has_typed_props_hygiene_entities_fragments_and_svg() {
    Fixture::compile(r#"
import macro {html} from "std/html_macro";
import {Html,text} from "std/html";
import {mount_ui,Ui} from "std/ui";
type Msg=Ignore;
fn node()->i16{99}
fn update(model:i16,msg:Msg,ui:Ui<Msg>)->i16{model}
fn view(model:&i16)->Html<Msg>{
 let mut order:i16=0;let step=fn()=>{order+=1;from_char((48+order as u32) as char)};
 let tree:Html<Msg>=html!{<><p title={step()} data-order={step()}>&amp;&#x1F30D;&lt;{text(step())}</p><svg><circle r="4"/></svg></>};
 println(order);tree
}
fn main(){mount_ui(0,update,view,1).unwrap();}
"#.replace("Html<Msg>=", "Html<Msg> =").as_str()).run(r#"
assert.equal(output.join(''),'3\n');assert.equal(dom.root.textContent,'&🌍<3');
const p=dom.root.childNodes[0];assert.equal(p.attributes.get('title'),'1');assert.equal(p.attributes.get('data-order'),'2');
assert.equal(dom.root.childNodes[1].childNodes[0].namespaceURI,'http://www.w3.org/2000/svg');
app.dispose();assert.equal(app.stats.liveAllocations,1);assert.deepEqual(errors,[]);
"#);
}
#[test]
fn public_html_macro_reports_schema_and_message_type_errors() {
    for expression in [
        "html!{<script/>}",
        "html!{<p onclick={Click}/>}",
        "html!{<input checked={\"wrong\"}/>}",
        "html!{<button on_click={42}/>}",
        "html!{<p>&#xD800;</p>}",
        "html!{<p></div>}",
    ] {
        let path = std::env::temp_dir().join(format!(
            "rfl-html-error-{}-{}.rfl",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::write(&path,format!("import macro {{html}} from \"std/html_macro\";import {{Html}} from \"std/html\";type Msg=Click;fn main(){{let view:Html<Msg>={expression};}}" )).unwrap();
        let result = rfl::check_file(&path);
        std::fs::remove_file(&path).unwrap();
        assert!(result.is_err(), "accepted {expression}");
    }
}

#[test]
fn owned_messages_queue_limits_fifo_and_followup_turns_preserve_single_ownership() {
    Fixture::compile(&source("tests/fixtures/browser-ui-owned.rfl")).run(r#"
assert.equal(output.join(''),'reentrant full\nfull\nmessage dropped\n');
assert.equal(dom.root.textContent,'297');assert.equal(effectTimers.size,1);
effectStep(0);assert.equal(dom.root.textContent,'298');assert.equal(effectTimers.size,0);
assert.equal(app.stats.commits,3);app.dispose();assert.equal(app.stats.liveAllocations,1);assert.deepEqual(errors,[]);
"#);
}

#[test]
fn pure_commands_receive_capabilities_and_fetch_results_become_typed_messages() {
    Fixture::compile(&source("tests/fixtures/browser-ui-command-fetch.rfl")).run(r#"
assert.equal(dom.root.textContent,'waiting');assert.equal(output.join(''),'builder dropped\n');
assert.equal(app.stats.effectsStarted,0);effectStep(0);assert.equal(app.stats.effectsStarted,1);
for(let i=0;i<20;++i)await Promise.resolve();effectStep(0);
assert.equal(dom.root.textContent,'fetched');assert.equal(app.stats.commits,2);assert.equal(app.stats.effectBatches,2);
assert.equal(effectTimers.size,0);assert.equal(app.stats.outstandingEffects,0);
app.dispose();assert.equal(app.stats.liveAllocations,1);assert.deepEqual(errors,[]);
"#);
}

#[test]
fn renderer_identity_is_move_only() {
    let path = std::env::temp_dir().join(format!("rfl-renderer-owner-{}.rfl", std::process::id()));
    std::fs::write(&path,"import {Renderer,renderer} from \"std/browser_render\";fn main(){let view:Renderer<i16> = renderer();let copy=view;drop(move(copy));drop(move(view));}").unwrap();
    let result = rfl::check_file(&path);
    std::fs::remove_file(&path).unwrap();
    let error = result.unwrap_err();
    assert!(error.detail.contains("move-only"), "{error}");
}

#[test]
fn html_children_convert_borrowed_scalars_and_composite_views_inside_wasm() {
    Fixture::compile(&source("tests/fixtures/browser-ui-children.rfl")).run(r#"
const section=dom.root.childNodes[0],paragraph=section.childNodes[0];
assert.equal(paragraph.textContent,'42|-128|255|65535|-70000|4000000000|-9223372036854775808|18446744073709551615|64000|true|false|🌍|<script>& 🌍');
assert.ok(paragraph.childNodes.every(node=>node.tagName===null));
assert.equal(section.childNodes[1].textContent,'AB');assert.equal(section.childNodes[2].textContent,'present');
assert.equal(section.childNodes[3].tagName,'strong');assert.equal(section.childNodes[3].textContent,'12');
assert.equal(section.childNodes[4].tagName,'em');assert.equal(section.childNodes[4].textContent,'64000');
assert.equal(section.childNodes[5].textContent,'once');assert.equal(section.childNodes[6].textContent,' legacy');assert.equal(section.childNodes[7].textContent,'123');
assert.equal(section.childNodes[8].textContent,' conditional');assert.equal(section.childNodes[9].textContent,' block');assert.equal(section.childNodes[10].textContent,' match');assert.equal(section.childNodes[11].textContent,'5');
assert.equal(output.join(''),'1\n');assert.equal(app.stats.commits,1);
assert.equal(app.stats.hostCalls,app.stats.commits+app.stats.outputCalls);assert.equal(app.stats.effectCommands,0);
app.dispose();assert.equal(app.stats.liveAllocations,1);assert.deepEqual(errors,[]);
"#);
}

#[test]
fn html_child_conversions_preserve_message_types_and_reject_unformatted_values() {
    for declaration in [
        "let other:Html<Other> = html!{<button on_click={Foreign}/>};let view:Html<Msg> = html!{<p>{other}</p>};",
        "let other:Vec<Html<Other>> = [html!{<button on_click={Foreign}/>}];let view:Html<Msg> = html!{<p>{other}</p>};",
        "let secret=Secret{value:1};let view:Html<Msg> = html!{<p>{secret}</p>};",
        "let n:f64=1.5;let view:Html<Msg> = html!{<p>{n}</p>};",
    ] {
        let path = std::env::temp_dir().join(format!("rfl-html-child-error-{}-{}.rfl",std::process::id(),std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()));
        std::fs::write(&path,format!("import macro {{html}} from \"std/html_macro\";import {{Html}} from \"std/html\";type Msg=Click;type Other=Foreign;struct Secret{{value:i16}}fn main(){{{declaration}}}")).unwrap();
        let result=rfl::check_file(&path);std::fs::remove_file(path).unwrap();
        assert!(result.is_err(),"accepted {declaration}");
    }
}

#[test]
fn todomvc_routes_edits_persistence_failures_and_ownership_execute_in_wasm() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    let fixture = Fixture::compile_path(&root.join("apps/todomvc/main.rfl"));
    let output =
        std::process::Command::new(std::env::var_os("NODE").unwrap_or_else(|| "node".into()))
            .arg("--test")
            .arg(root.join("../rcc-wasm/browser/todomvc.test.mjs"))
            .env("RIPPLE_TODO_WASM", fixture.0.join("app.wasm"))
            .output()
            .unwrap();
    assert!(
        output.status.success(),
        "{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn todo_codec_and_typed_routes_reject_malformed_input_on_native_and_ripple() {
    let path =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/todomvc-codec.rfl");
    for output in [
        rfl::run_native_file(&path, false).unwrap(),
        rfl::run_file(&path).unwrap(),
    ] {
        assert_eq!(output.status, 0);
        assert_eq!(output.stdout, "codec and typed routes passed\n");
    }
}

#[test]
fn browser_platform_roundtrip_owns_its_subscription_and_utf8_responses() {
    Fixture::compile(&source("tests/fixtures/browser-platform.rfl")).run(r#"
const {testPlatform}=await import(new URL('./test-platform.mjs',pathToFileURL(process.argv[2])));
// The shared fixture mounted without location/storage services; use a dedicated
// instance so the full platform behavior runs against deterministic host services.
app.dispose();const host=testPlatform('#/initial');const nextDOM=testDOM(),values=[],failures=[];
const next=await mountBrowser(readFileSync(process.argv[1]),{...nextDOM,effectHost:host,onOutput:t=>values.push(t),onError:e=>failures.push(e)});
host.flush();assert.equal(values.join(''),'hé 🌍\n');assert.equal(nextDOM.root.textContent,'#/done');assert.deepEqual(failures,[]);
assert.equal(next.stats.locationSubscriptions,1);next.dispose();assert.equal(host.listenerCount,0);assert.equal(next.stats.liveAllocations,1);
"#);
}

#[test]
fn focus_waits_for_insertion_across_chunks_and_does_not_repeat_on_unchanged_views() {
    Fixture::compile(&source("tests/fixtures/browser-focus.rfl")).run(r#"
const section=dom.root.childNodes[0],automatic=section.childNodes[0],manual=section.childNodes.at(-1);
assert.ok(app.stats.commits>=3);assert.equal(dom.document.activeElement,automatic);
assert.equal(automatic.selectionStart,0);assert.equal(automatic.selectionEnd,8);dom.frame();
manual.focus();const before=app.stats;automatic.dispatch('keydown',{key:'Shift'});dom.frame();
assert.equal(dom.document.activeElement,manual);assert.equal(app.stats.commits,before.commits);assert.equal(output.join(''),'selected\n');
app.dispose();assert.equal(app.stats.liveAllocations,1);assert.deepEqual(errors,[]);
"#);
}
