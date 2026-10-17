//! Checked stdlib extraction must preserve value/owner semantics on both targets.
use std::path::PathBuf;

struct Program(PathBuf);
impl Program {
    fn new(source: &str) -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let dir = std::env::temp_dir().join(format!(
            "rfl-extraction-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("main.rfl");
        std::fs::write(&path, source).unwrap();
        Self(path)
    }
}
impl Drop for Program {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(self.0.parent().unwrap());
    }
}
fn parity(source: &str, status: i32, stdout: &str) {
    let program = Program::new(source);
    for run in [
        rfl::run_native_file(&program.0, false),
        rfl::run_file(&program.0),
    ] {
        let run = run.unwrap_or_else(|e| panic!("{e}"));
        assert_eq!(run.status, status, "{}", run.stdout);
        assert_eq!(run.stdout, stdout);
    }
}

#[test]
fn extract_scalars_unit_managed_products_and_callables_without_formatting_bounds() {
    parity(
        r#"
        import { Option } from "std/option";
        import { Result } from "std/result";
        struct Error {code:i16}
        struct Data {text:String, values:Vec<i16>}
        fn number()->Result<i16,Error> {Ok(42)}
        fn message()->String {println("message evaluated");"unused message"}
        fn main()->i16 {
            let n=Some(17).unwrap();
            if (n!=17) {return 1;}
            Some({}).expect("unit");
            let u:Result<unit,Error> =Ok({}); u.unwrap();
            let text=Some("hello").unwrap();
            println(text);
            let data:Result<Data,Error> =Ok(Data{text:"data",values:[10,20]});
            let data=data.expect(message());
            if (data.text!="data" || data.values[1]!=20) {return 2;}
            let call=Some(fn(x:i16)=>x+1).unwrap();
            if (call(9)!=10) {return 3;}
            let nested:Option<Result<i16,Error>> =Some(number());
            nested.unwrap().unwrap()
        }
    "#,
        42,
        "hello\nmessage evaluated\n",
    );
}

#[test]
fn extraction_transfers_resource_outputs_and_preserves_ordinary_aliases() {
    parity(
        r#"
        import { Option } from "std/option";
        import { Result } from "std/result";
        fn token(n:i16)->Owned<i16> {owned(n,fn(p:&mut i16)=>{println(*p);})}
        fn main()->i16 {
            let text=Some("alive");
            let copy=text.unwrap();
            println(text.unwrap());println(copy);
            let option=Some(token(1));
            let owner=move(option).unwrap();
            println("owner alive");drop(move(owner));
            let result:Result<Owned<i16>,Owned<i16>> =Ok(token(2));
            let owner=move(result).expect("resource");
            drop(move(owner));
            let mut values:Vec<Owned<i16>> =[];
            for (n in 3..7) {values.push(token(n));}
            while (!values.is_empty()) {let owner=values.pop().unwrap();drop(move(owner));}
            42
        }
    "#,
        42,
        "alive\nalive\nowner alive\n1\n2\n6\n5\n4\n3\n",
    );
}

#[test]
fn extracted_futures_can_suspend_and_return_owned_results() {
    parity(
        r#"
        import { Result } from "std/result";
        import {Future, yield_now, sleep} from "std/future";
        import {Reactor, block_on} from "std/executor";
        struct Clock {time:u32}
        impl Reactor for Clock {
            fn now(&self)->u32 {self.time}
            fn turn(&mut self,wait:Option<u32>)->bool {
                match wait {Some(t)=>{self.time+=t;},None=>{}}true
            }
        }
        async fn work()->Result<Owned<i16>,unit> {
            sleep(5).unwrap().await;
            yield_now().await;
            Ok(owned(42,fn(p:&mut i16)=>{println(*p);}))
        }
        fn main()->i16 {
            let mut clock=Clock{time:0};
            let mut f=work();
            let value=block_on(&mut f,&mut clock).expect("executor").unwrap();
            drop(move(f));
            if (clock.time!=5) {return 1;}
            println("output survives");drop(move(value));42
        }
    "#,
        42,
        "output survives\n42\n",
    );
}

#[test]
fn failure_reports_default_or_dynamic_message_and_never_continues() {
    for (expression, message) in [
        ("none().unwrap()", "called Option.unwrap() on None"),
        ("failure().unwrap()", "called Result.unwrap() on Err"),
        (
            "none().expect(\"timer \"+\"unavailable\")",
            "timer unavailable",
        ),
        ("failure().expect(\"ошибка: café\")", "ошибка: café"),
        ("none().expect(\"\")", ""),
    ] {
        let source = format!(
            r#"
            import {{ Option }} from "std/option";
            import {{ Result }} from "std/result";
            struct Error {{code:i16}}
            fn none()->Option<i16> {{None}}
            fn failure()->Result<i16,Error> {{Err(Error{{code:9}})}}
            fn main()->i16 {{
                let owner=owned(1,fn(p:&mut i16)=>{{println("unexpected unwind");}});
                let n={expression};
                println("unexpected continuation");42
            }}
        "#
        );
        parity(&source, 1, &format!("error: {message}\n"));
    }
}

#[test]
fn checking_rejects_invalid_messages_arity_and_copies_of_resource_receivers() {
    for (body, expected) in [
        ("Some(1).expect(4);", "String"),
        ("Some(1).unwrap(2);", "expects 0 arguments"),
        ("Some(1).expect();", "expects 1 arguments"),
        ("let n:bool=Some(1).unwrap();", "expected bool"),
        (
            "let a=Some(owned(1,fn(p:&mut i16)=>{}));let b=a.unwrap();",
            "move-only resource cannot be copied",
        ),
        (
            "let a=Some(owned(1,fn(p:&mut i16)=>{}));let b=move(a).unwrap();drop(move(a));",
            "use of a moved value",
        ),
    ] {
        let p = Program::new(&format!(
            r#"
            import {{ Option }} from "std/option";
            fn main() {{{body}}}
        "#
        ));
        let error = rfl::check_file(&p.0).unwrap_err().to_string();
        assert!(error.contains(expected), "expected {expected}: {error}");
    }
}

#[test]
fn default_diagnostics_do_not_allocate_a_string_on_the_success_path() {
    use rcc_ir::ir::{Instruction, Value};
    let p = Program::new(
        r#"
        import { Option } from "std/option";
        fn main()->i16 {Some(42).unwrap()}
    "#,
    );
    let module = rfl::compile_file(&p.0).unwrap();
    let unwrap = module
        .functions
        .iter()
        .find(|f| f.name.contains("unwrap"))
        .unwrap();
    let calls: Vec<_> = unwrap
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter_map(|i| match i {
            Instruction::Call {
                function: Value::Global(name),
                ..
            } => Some(name.as_str()),
            _ => None,
        })
        .collect();
    assert!(
        !calls.contains(&"malloc"),
        "unwrap allocates its diagnostic: {calls:?}"
    );
    assert!(
        calls.contains(&"rfl_halt"),
        "failed extraction must terminate"
    );
}
