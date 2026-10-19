use rcc_ir::target::Target;
use rcc_wasm::{compile, Options};
use std::{fs, path::PathBuf, process::Command};

fn c(source: &str) -> rcc_ir::ir::Module {
    let source = rcc_preprocessor::Preprocessor::new()
        .process(source, PathBuf::from("browser-test.c"))
        .unwrap();
    let mut module = rcc_frontend::Frontend::compile_to_ir(&source, "browser-test").unwrap();
    module.target = Target::Wasm;
    module
}
#[test]
fn browser_protocol_and_real_wasm_lifecycle() {
    let source = format!(
        "{}\n{}",
        include_str!("../browser.h"),
        include_str!("../examples/browser-counter.c").replace("#include \"browser.h\"", "")
    );
    let artifact = compile(&[c(&source)], &Options::default()).unwrap();
    let path = std::env::temp_dir().join(format!("rcc-wasm-browser-{}.wasm", std::process::id()));
    fs::write(&path, &artifact.bytes).unwrap();
    let stress = compile(
        &[c(&format!(
            "{}\n{}",
            include_str!("../browser.h"),
            include_str!("fixtures/browser-stress.c")
        ))],
        &Options::default(),
    )
    .unwrap();
    let stress_path = path.with_extension("stress.wasm");
    fs::write(&stress_path, &stress.bytes).unwrap();
    let output = Command::new(std::env::var_os("NODE").unwrap_or_else(|| "node".into()))
        .arg("--test")
        .arg(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("browser/bridge.test.mjs"))
        .env("RIPPLE_TEST_WASM", &path)
        .env("RIPPLE_STRESS_WASM", &stress_path)
        .output()
        .unwrap();
    fs::remove_file(&path).unwrap();
    fs::remove_file(&stress_path).unwrap();
    assert!(
        output.status.success(),
        "{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}
#[test]
fn browser_requires_explicit_correctly_typed_lifecycle_hooks() {
    for source in [
        "extern void ripple_browser_mount(void); int main(void){ripple_browser_mount();return 0;}",
        "extern void ripple_browser_mount(void); int main(void){ripple_browser_mount();return 0;} int ripple_browser_on_events(int n){return n;} void ripple_browser_on_dispose(void){}",
    ] {
        assert!(compile(&[c(source)], &Options::default()).is_err());
    }
}

#[test]
fn dom_only_browser_code_has_no_console_import() {
    let source = format!(
        "{}\n{}",
        include_str!("../browser.h"),
        r#"
int main(void){ripple_browser_mount();ripple_dom_begin();ripple_dom_command(2,1,ripple_dom_string((const unsigned short*)"Y",1),0,0);ripple_dom_command(6,0,1,0,0);ripple_dom_commit();return 0;}
void ripple_browser_on_events(const unsigned short *p,unsigned long n){}
void ripple_browser_on_dispose(void){}
"#
    );
    let artifact = compile(&[c(&source)], &Options::default()).unwrap();
    let mut imports = Vec::new();
    for payload in wasmparser::Parser::new(0).parse_all(&artifact.bytes) {
        if let wasmparser::Payload::ImportSection(reader) = payload.unwrap() {
            for import in reader {
                let import = import.unwrap();
                imports.push((import.module.to_string(), import.name.to_string()));
            }
        }
    }
    assert_eq!(imports, vec![("ripple".into(), "dom_commit".into())]);
    let path = std::env::temp_dir().join(format!("rcc-wasm-dom-only-{}.wasm", std::process::id()));
    fs::write(&path, &artifact.bytes).unwrap();
    let script = r#"const fs=require('node:fs');let calls=0;const i=new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(process.argv[1])),{ripple:{dom_commit(a,n){if(typeof a!=='number'||n!==26)throw Error('bad typed import');++calls;}}});if(i.exports.__ripple_start()!==0||calls!==1)throw Error('bad startup');i.exports.__ripple_dispose();"#;
    let output = Command::new(std::env::var_os("NODE").unwrap_or_else(|| "node".into()))
        .args(["-e", script])
        .arg(&path)
        .output()
        .unwrap();
    fs::remove_file(&path).unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn browser_effects_execute_in_real_wasm() {
    let artifact = compile(
        &[c(&format!(
            "{}\n{}",
            include_str!("../browser.h"),
            include_str!("fixtures/browser-effects.c")
        ))],
        &Options::default(),
    )
    .unwrap();
    let path = std::env::temp_dir().join(format!("rcc-wasm-effects-{}.wasm", std::process::id()));
    fs::write(&path, &artifact.bytes).unwrap();
    let capacity = compile(
        &[c(&format!(
            "{}\n{}",
            include_str!("../browser.h"),
            include_str!("fixtures/browser-effect-capacity.c")
        ))],
        &Options::default(),
    )
    .unwrap();
    let capacity_path = path.with_extension("capacity.wasm");
    fs::write(&capacity_path, &capacity.bytes).unwrap();
    let output = Command::new(std::env::var_os("NODE").unwrap_or_else(|| "node".into()))
        .arg("--test")
        .arg(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("browser/effects.test.mjs"))
        .arg(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("browser/task-pump.test.mjs"))
        .arg(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("browser/platform.test.mjs"))
        .env("RIPPLE_EFFECT_WASM", &path)
        .env("RIPPLE_EFFECT_CAPACITY_WASM", &capacity_path)
        .output()
        .unwrap();
    fs::remove_file(&path).unwrap();
    fs::remove_file(&capacity_path).unwrap();
    assert!(
        output.status.success(),
        "{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn mounted_executor_requires_a_correctly_typed_turn_hook() {
    let source = format!("{}\n{}", include_str!("../browser.h"),
        "int main(void){ripple_browser_mount();ripple_browser_effects();ripple_browser_executor();return 0;}void ripple_browser_on_events(const unsigned short *p,unsigned long n){}void ripple_browser_on_dispose(void){}void ripple_browser_on_effects(void){}");
    assert!(compile(&[c(&source)], &Options::default()).is_err());
    let wrong = format!(
        "{}\nunsigned ripple_browser_on_turn(void){{return 0;}}",
        source.replace(
            "unsigned long ripple_browser_on_turn(void);",
            "unsigned ripple_browser_on_turn(void);"
        )
    );
    assert!(compile(&[c(&wrong)], &Options::default()).is_err());
    let correct =
        format!("{source}\nunsigned long ripple_browser_on_turn(void){{return 4294967295UL;}}");
    let artifact = compile(&[c(&correct)], &Options::default()).unwrap();
    let path = std::env::temp_dir().join(format!(
        "rcc-wasm-executor-clock-{}.wasm",
        std::process::id()
    ));
    fs::write(&path, &artifact.bytes).unwrap();
    let script = r#"const assert=require('node:assert/strict'),fs=require('node:fs');
const module=new WebAssembly.Module(fs.readFileSync(process.argv[1]));
const instance=()=>new WebAssembly.Instance(module,{ripple:{effect_commit(){}}});
const i=instance();assert.equal(i.exports.__ripple_start(4294967290),0);
i.exports.__ripple_turn(4);i.exports.__ripple_turn(4);i.exports.__ripple_dispose();
const j=instance();j.exports.__ripple_start(100);
assert.throws(()=>j.exports.__ripple_turn(99),WebAssembly.RuntimeError);assert.equal(j.exports.__ripple_fault.value,8);
const k=instance();k.exports.__ripple_start(0);
assert.throws(()=>k.exports.__ripple_turn(2147483648),WebAssembly.RuntimeError);assert.equal(k.exports.__ripple_fault.value,8);
"#;
    let output = Command::new(std::env::var_os("NODE").unwrap_or_else(|| "node".into()))
        .args(["-e", script])
        .arg(&path)
        .output()
        .unwrap();
    fs::remove_file(&path).unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn effects_only_use_one_typed_import_and_require_a_completion_hook() {
    let main = "int main(void){ripple_browser_mount();ripple_browser_effects();ripple_effect_release(ripple_effect_timer(0));return 0;}void ripple_browser_on_events(const unsigned short *p,unsigned long n){}void ripple_browser_on_dispose(void){}";
    let source = format!(
        "{}\n{main}\nvoid ripple_browser_on_effects(void){{}}",
        include_str!("../browser.h")
    );
    let artifact = compile(&[c(&source)], &Options::default()).unwrap();
    let mut imports = Vec::new();
    for payload in wasmparser::Parser::new(0).parse_all(&artifact.bytes) {
        if let wasmparser::Payload::ImportSection(reader) = payload.unwrap() {
            for import in reader {
                let import = import.unwrap();
                imports.push((import.module.to_owned(), import.name.to_owned()));
            }
        }
    }
    assert_eq!(
        imports,
        vec![("ripple".to_owned(), "effect_commit".to_owned())]
    );
    let missing = format!("{}\n{main}", include_str!("../browser.h"));
    assert!(compile(&[c(&missing)], &Options::default()).is_err());
    let wrong = format!(
        "{}\n{main}\nint ripple_browser_on_effects(void){{return 0;}}",
        include_str!("../browser.h").replace(
            "void ripple_browser_on_effects(void);",
            "int ripple_browser_on_effects(void);"
        )
    );
    assert!(compile(&[c(&wrong)], &Options::default()).is_err());
}

#[test]
fn platform_completion_validation_is_atomic_and_location_consumption_needs_no_host_ack() {
    let artifact = compile(
        &[c(&format!(
            "{}\n{}",
            include_str!("../browser.h"),
            include_str!("fixtures/browser-platform.c")
        ))],
        &Options::default(),
    )
    .unwrap();
    let path = std::env::temp_dir().join(format!("rcc-wasm-platform-{}.wasm", std::process::id()));
    fs::write(&path, &artifact.bytes).unwrap();
    let script = r#"
const assert=require('node:assert/strict'),fs=require('node:fs');
const module=new WebAssembly.Module(fs.readFileSync(process.argv[1]));
function mount(){
 let instance,output='',commits=0;
 instance=new WebAssembly.Instance(module,{ripple:{effect_commit(){++commits;},write(address,cells){const words=new Uint16Array(instance.exports.memory.buffer,address,cells);output+=String.fromCharCode(...words);}}});
 instance.exports.__ripple_start();
 return {instance,get output(){return output;},get commits(){return commits;}};
}
function deliver(app,records){
 const words=[0x5247,1,4+records.reduce((n,r)=>n+r.length,0),records.length,...records.flat()];
 const address=app.instance.exports.__ripple_effect_buffer(),view=new DataView(app.instance.exports.memory.buffer);
 words.forEach((word,i)=>view.setUint16(address+i*2,word,true));app.instance.exports.__ripple_completions(words.length);
}
const good=[11,1,0,4,0,3,0,0,35,47,120],app=mount();
deliver(app,[good]);deliver(app,[good]);assert.equal(app.output,'YY');assert.equal(app.commits,1);
app.instance.exports.__ripple_dispose();assert.equal(app.commits,1);
for(const bad of [
 [8,1,0,4,1,0,0,0], [8,1,0,4,0,0,1,0],
 [8,1,0,4,0,0,5,0], [9,1,0,5,0,1,0,0,120],
 [9,1,0,6,0,1,0,0,120], [11,1,0,6,1,3,0,0,35,47,120],
 [8,1,0,3,0,0,0,0], [2057,1,0,4,0,2049,0,0,...Array(2049).fill(120)],
]){
 const broken=mount();assert.throws(()=>deliver(broken,[good,bad]),WebAssembly.RuntimeError);
 assert.equal(broken.instance.exports.__ripple_fault.value,8);assert.equal(broken.output,'');
 const metrics=broken.instance.exports.__ripple_heap_metrics();
 assert.equal(new DataView(broken.instance.exports.memory.buffer).getUint32(metrics+4,true),0);
}
"#;
    let output = Command::new(std::env::var_os("NODE").unwrap_or_else(|| "node".into()))
        .args(["-e", script])
        .arg(&path)
        .output()
        .unwrap();
    fs::remove_file(path).unwrap();
    assert!(
        output.status.success(),
        "{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}
