//! Execute actual modules in Node's WebAssembly engine. Set NODE to select it.
use rcc_ir::{ir::*, target::Target};
use rcc_wasm::{compile, Artifact, Options};
use std::{
    fs,
    process::Command,
    sync::atomic::{AtomicU64, Ordering},
};

type Case = (String, Vec<String>, String);
fn run(artifact: &Artifact, cases: Vec<Case>) {
    static ID: AtomicU64 = AtomicU64::new(0);
    let path = std::env::temp_dir().join(format!(
        "rcc-wasm-test-{}-{}.wasm",
        std::process::id(),
        ID.fetch_add(1, Ordering::Relaxed)
    ));
    fs::write(&path, &artifact.bytes).unwrap();
    let script = r#"
const fs = require('node:fs');
const bytes = fs.readFileSync(process.argv[1]);
if (WebAssembly.Module.imports(new WebAssembly.Module(bytes)).length !== 0)
    throw Error('pure computation must not cross into JS');
const instance = new WebAssembly.Instance(new WebAssembly.Module(bytes), {});
const cases = JSON.parse(fs.readFileSync(0, "utf8"));
for (const [name, args, expected] of cases) {
    const got = BigInt.asUintN(64, instance.exports[name](...args.map(BigInt))).toString();
    if (got !== expected) throw Error(`${name}(${args}) => ${got}, expected ${expected}`);
}
console.log(cases.length);
"#;
    use std::{io::Write, process::Stdio};
    // The numeric matrix exceeds Linux's per-argument limit; stream test data.
    let mut child = Command::new(std::env::var_os("NODE").unwrap_or_else(|| "node".into()))
        .args(["-e", script])
        .arg(&path)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .expect("Node is required for Wasm execution tests (or set NODE)");
    child
        .stdin
        .take()
        .unwrap()
        .write_all(&serde_json::to_vec(&cases).unwrap())
        .unwrap();
    let result = child.wait_with_output();
    fs::remove_file(&path).unwrap();
    let output = result.expect("Node is required for Wasm execution tests (or set NODE)");
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert_eq!(
        String::from_utf8_lossy(&output.stdout).trim(),
        cases.len().to_string()
    );
}
fn module(functions: Vec<Function>) -> Module {
    let mut m = Module::new("test".into());
    m.target = Target::Wasm;
    m.functions = functions;
    m
}
fn block(id: u32, instructions: Vec<Instruction>) -> BasicBlock {
    let mut b = BasicBlock::new(id);
    b.instructions = instructions;
    b
}
fn parse(body: &str) -> Module {
    rcc_ir::parse_module(&format!("module test {{ target wasm {body} }}")).unwrap()
}
fn options(names: &[&str]) -> Options {
    Options {
        exports: names.iter().map(|s| s.to_string()).collect(),
    }
}
fn case(name: &str, args: &[u64], expected: u64) -> Case {
    (
        name.into(),
        args.iter().map(u64::to_string).collect(),
        expected.to_string(),
    )
}

#[path = "cases/arithmetic.rs"]
mod arithmetic;
#[path = "cases/control_flow.rs"]
mod control_flow;
#[path = "cases/linking.rs"]
mod linking;
#[path = "cases/validation.rs"]
mod validation;

#[path = "cases/floats.rs"]
mod floats;
#[path = "cases/memory.rs"]
mod memory;
fn c(source: &str) -> Module {
    let mut module = rcc_frontend::Frontend::compile_to_ir(source, "c-test").unwrap();
    module.target = Target::Wasm;
    module
}
fn traps(artifact: &Artifact, cases: Vec<Case>) {
    static ID: AtomicU64 = AtomicU64::new(0);
    let path = std::env::temp_dir().join(format!(
        "rcc-wasm-traps-{}-{}.wasm",
        std::process::id(),
        ID.fetch_add(1, Ordering::Relaxed)
    ));
    fs::write(&path, &artifact.bytes).unwrap();
    let script = r#"
const fs=require('node:fs');const module=new WebAssembly.Module(fs.readFileSync(process.argv[1]));
for(const [name,args,code] of JSON.parse(process.argv[2])) {
 const instance=new WebAssembly.Instance(module,{});
 let trapped=false;
 try {instance.exports[name](...args.map(BigInt));} catch(e) {if(!(e instanceof WebAssembly.RuntimeError))throw e; trapped=true;}
 if(!trapped || instance.exports.__ripple_fault.value!==Number(code)) throw Error(`${name}: expected trap ${code}, got ${instance.exports.__ripple_fault.value}`);
}
"#;
    let output = Command::new(std::env::var_os("NODE").unwrap_or_else(|| "node".into()))
        .args(["-e", script])
        .arg(&path)
        .arg(serde_json::to_string(&cases).unwrap())
        .output()
        .unwrap();
    fs::remove_file(path).unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
}
#[path = "cases/output.rs"]
mod output;
