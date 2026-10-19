fn reject(source: &str, message: &str) {
    let error = rfl::check_source(source).expect_err(source);
    assert!(
        error.message.contains(message),
        "expected {message}: {error}"
    );
}
fn parity(source: &str, expected: &str) {
    for result in [
        rfl::run_native_program(source).unwrap(),
        rfl::run_program(source).unwrap(),
    ] {
        assert_eq!(result.status, 0, "{}", result.stdout);
        assert_eq!(result.stdout, expected);
    }
}
const BASIC: &str = r#"
    fn invoke(f:once fn(i16)->String)->String {f(42)}
    fn main(){
        let value=owned("owned",fn(x:&mut String)=>{println("drop");});
        let f=once fn(x:i16)=>{println(x);println(*value.borrow());};
        f(21);
        println(invoke(once fn(x:i16)=>"called once"));
    }
"#;
#[test]
fn consuming_callables_capture_and_call() {
    rfl::check_source(BASIC).unwrap();
}
#[test]
fn consuming_callables_execute_on_both_targets() {
    parity(BASIC, "21\nowned\ndrop\ncalled once\n");
}
#[test]
fn consuming_callables_reject_copies_repeated_calls_and_reuse_of_captures() {
    reject("fn main(){let f=once fn()=>42;f();f();}", "moved");
    reject(
        "fn main(){let f=once fn()=>42;let g=f;}",
        "cannot be copied",
    );
    reject(
        "fn main(){let x=owned(42,fn(x:&mut i16)=>{});let f=once fn()=>drop(move(x));x.borrow();}",
        "moved",
    );
    reject(
        "fn reuse(f:fn()->i16){f();f();} fn main(){reuse(once fn()=>42);}",
        "reusable",
    );
}

#[test]
fn unused_nested_and_returned_captures_drop_exactly_once() {
    parity(
        r#"
        fn token(n:i16)->Owned<i16>{owned(n,fn(n:&mut i16)=>{println(*n);})}
        fn make(n:i16)->once fn()->Owned<i16>{let t=token(n);once fn()=>move(t)}
        fn ignore(f:once fn()->Owned<i16>){}
        fn main(){
            {let a=token(1);let b=token(2);let f=once fn()=>{a.borrow();b.borrow();};}
            let f=make(3);let t=f();println("returned");drop(move(t));
            ignore(make(4));
            let inner=make(5);let outer=once fn()=>inner();let t=outer();drop(move(t));
            let inner=make(6);let outer=once fn()=>inner();drop(move(outer));
            let x="snapshot";let f=once fn()=>x;println(f());
        }
    "#,
        "2\n1\nreturned\n3\n4\n5\n6\nsnapshot\n",
    );
}

#[test]
fn captures_can_be_mutated_transferred_and_reinitialized() {
    parity(
        r#"
        fn main(){
            let mut n=40;let f=once fn()=>{n+=2;n};println(f());
            n=100;println(n);
            let mut t=owned(41,fn(n:&mut i16)=>{println(*n);});
            let f=once fn()=>{let p=t.borrow_mut();*p+=1;};f();
            let mut f=once fn()=>1;println(f());f=once fn()=>2;println(f());
            let f=once fn()=>3;let g=move(f);println(g());
        }
    "#,
        "42\n100\n42\n1\n2\n3\n",
    );
}

#[test]
fn control_flow_rejects_repeated_consumption_and_capture_after_move() {
    reject("fn main(){let f=once fn()=>1;if(true){f();}f();}", "moved");
    reject(
        "fn main(){let f=once fn()=>1;while(true){f();}}",
        "repeating loop",
    );
    reject(
        "fn main(){let x=1;while(true){let f=once fn()=>x;f();}}",
        "repeating loop",
    );
    reject(
        "fn main(){let x=1;let f=once fn()=>x;let g=once fn()=>x;}",
        "moved",
    );
    reject("fn main(){let f=once fn()=>1;drop(move(f));f();}", "moved");
    reject("fn main(){let f=once fn()=>1;let g=fn()=>f();}", "reusable");
    reject(
        "fn main(){let x=owned(1,fn(x:&mut i16)=>{});let g=fn()=>x.borrow();}",
        "reference",
    );
    rfl::check_source("fn main(){let f=once fn()=>1;if(true){f();}else{f();}}").unwrap();
    rfl::check_source("fn main(){let mut f=once fn()=>1;for(n in 0..2){f();f=once fn()=>2;}}")
        .unwrap();
}

#[test]
fn references_cannot_escape_and_live_loans_block_capture_or_call() {
    reject(
        "fn main(){let n=1;let p=&n;let f=once fn()=>*p;}",
        "capture a reference",
    );
    reject("fn main(){let n=owned(1,fn(x:&mut i16)=>{});let p=n.borrow();let f=once fn()=>drop(move(n));println(*p);}","borrow");
    reject("fn main(){let f=once fn()=>1;let p=&f;f();}", "borrow");
    reject("fn main(){let mut n=owned(1,fn(x:&mut i16)=>{});let p=n.borrow_mut();let f=once fn()=>drop(move(n));}","borrow");
}

#[test]
fn generic_types_inference_and_trait_contracts_preserve_cardinality() {
    parity(
        r#"
        type Box<T> = Box(T);
        fn id<T>(x:T)->T{move(x)}
        fn invoke<A,B>(x:A,f:once fn(A)->B)->B{f(move(x))}
        fn named(x:i16)->i16{x+1}
        fn main(){
            let reusable=id(fn(x:i16)=>x+1);println(reusable(1));println(reusable(2));
            let consuming=id(once fn(x:i16)=>x+2);println(consuming(40));
            println(invoke(41,named));
            let boxed=Box(once fn()=>43);match move(boxed){Box(f)=>println(f())};
            let f=once fn(x:&i16)=>*x;let n=44;println(f(&n));
        }
    "#,
        "2\n3\n42\n42\n43\n44\n",
    );
    reject(
        "fn wrong<T>(x:T,f:once fn(T)->T)->T{f(move(x));f(move(x))}",
        "moved",
    );
    reject(
        "trait C{fn go<T>(&self,f:once fn(T)->T);} impl C for bool{fn go<T>(&self,f:fn(T)->T){}}",
        "signature",
    );
    reject(
        "fn reusable<T>(x:T,f:fn(T)->T)->T{f(move(x))} fn main(){reusable(1,once fn(x:i16)=>x);}",
        "reusable",
    );
}

#[test]
fn early_return_and_question_mark_release_consuming_captures() {
    parity(
        r#"
        fn token(n:i16)->Owned<i16>{owned(n,fn(n:&mut i16)=>{println(*n);})}
        fn main(){
            let a=token(1);let f:once fn()->Result<i16,String> = once fn()=>{a.borrow();let x:Result<i16,String> = Err("stop");let n=x?;Ok(n)};
            println(f());
            let b=token(2);let g:once fn()->i16 = once fn()=>{b.borrow();return 42;};println(g());
        }
    "#,
        "1\nErr(stop)\n2\n42\n",
    );
}

struct Fixture(std::path::PathBuf);
impl Fixture {
    fn new(source: &str) -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let dir = std::env::temp_dir().join(format!(
            "rfl-once-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("main.rfl");
        std::fs::write(&path, source).unwrap();
        Self(path)
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(self.0.parent().unwrap());
    }
}
fn parity_file(source: &str, expected: &str) {
    let file = Fixture::new(source);
    for result in [
        rfl::run_native_file(&file.0, false).unwrap(),
        rfl::run_file(&file.0).unwrap(),
    ] {
        assert_eq!(result.status, 0, "{}", result.stdout);
        assert_eq!(result.stdout, expected);
    }
}

#[test]
fn one_shot_fp_short_circuits_and_releases_unused_owned_callbacks() {
    parity_file(
        r#"
        import {FunctorOnce,MonadOnce,map_once,flat_map_once,pure_once} from "std/fp";
        fn token(n:i16)->Owned<i16>{owned(n,fn(n:&mut i16)=>{println(*n);})}
        fn main(){
            let t=token(1);println(map_once<Option,_,_>(Some(41),once fn(x:i16)=>{t.borrow();x+1}));
            let t=token(2);println(map_once<Option,i16,i16>(None,once fn(x:i16)=>{println("bad");t.borrow();x}));
            let t=token(3);println(flat_map_once<Result<_,String>,i16,i16>(Err("stop"),once fn(x:i16)=>{println("bad");t.borrow();Ok(x)}));
            let t=token(4);let result:Result<Owned<i16>,String> = flat_map_once<Result<_,String>,i16,Owned<i16>>(Ok(42),once fn(x:i16)=>Ok(move(t)));
            match move(result){Ok(t)=>println(*t.borrow()),Err(_)=>{}};
            println(pure_once<Result<_,String>,_>(42));
        }
    "#,
        "1\nSome(42)\n2\nNone\n3\nErr(stop)\n4\n4\nOk(42)\n",
    );
}

#[test]
fn cancellation_releases_callbacks_before_or_after_suspension() {
    parity_file(
        r#"
        import {Result} from "std/result";
        import {Future,Pending,Ready,yield_now} from "std/future";
        import {Context,task_set} from "std/task";
        async fn work(f:once fn()->i16)->i16 {yield_now().await;f()}
        fn callback(n:i16)->once fn()->i16 {let t=owned(n,fn(n:&mut i16)=>{println(*n);});once fn()=>*t.borrow()}
        fn main()->i16 {
            let q=task_set(1,0).unwrap();let w=q.allocate().unwrap();let mut cx=Context{waker:w,now:0};
            let a=work(callback(1));drop(move(a));
            let mut b=work(callback(2));match b.poll(&mut cx){Pending=>{},_=>{return 1;}}b.cancel();b.cancel();drop(move(b));
            let mut c=work(callback(3));match c.poll(&mut cx){Pending=>{},_=>{return 2;}}match c.poll(&mut cx){Ready(n)=>println(n),_=>{return 3;}}
            0
        }
    "#,
        "1\n2\n3\n3\n",
    );
}

#[test]
fn consuming_syntax_formats_stably_and_once_remains_a_contextual_identifier() {
    let source="fn call(f:once fn(i16)->i16)->i16{f(42)} fn main(){let once=1;println(once);println(call(once fn(x:i16)=>x));}";
    let formatted = rfl::format_source(source).unwrap();
    assert!(formatted.contains("once fn"), "{formatted}");
    assert_eq!(rfl::format_source(&formatted).unwrap(), formatted);
    rfl::check_source(&formatted).unwrap();
}

#[test]
fn nested_callbacks_forward_only_lexically_free_captures() {
    parity(
        r#"
        fn main(){
            let value=owned(42,fn(x:&mut i16)=>{println(*x);});
            let outer=once fn()=>once fn()=>move(value);
            let inner=outer();let value=inner();drop(move(value));
            let value=owned(43,fn(x:&mut i16)=>{println(*x);});
            let outer=once fn()=>once fn()=>move(value);drop(move(outer));
            let value=7;let outer=fn()=>fn()=>value;let inner=outer();println(inner());println(inner());let inner=outer();println(inner());
        }
    "#,
        "42\n43\n7\n7\n7\n",
    );
    rfl::check_source("fn main(){let value=1;let f=once fn(value:i16)=>once fn()=>value;println(value);let g=f(42);println(g());}").unwrap();
    rfl::check_source("fn main(){let value=1;let f=once fn()=>{let value=2;once fn()=>value};println(value);let g=f();println(g());}").unwrap();
    reject(
        "fn main(){let value=owned(1,fn(x:&mut i16)=>{});let f=fn()=>once fn()=>move(value);}",
        "copyable closure",
    );
}

#[test]
fn consuming_capture_transfer_and_abandonment_run_on_the_brainfuck_cpu() {
    use std::process::Command;
    fn command(cmd: &mut Command) -> std::process::Output {
        let result = cmd.output().unwrap();
        assert!(
            result.status.success(),
            "{cmd:?}: {}",
            String::from_utf8_lossy(&result.stderr)
        );
        result
    }
    let source = r#"
        fn main(){
            let resource=owned(75,fn(x:&mut i16)=>{putc(*x);});
            let callback=once fn()=>{putc(79);move(resource)};
            let resource=callback();drop(move(resource));
            let resource=owned(33,fn(x:&mut i16)=>{putc(*x);});
            let unused=once fn()=>drop(move(resource));drop(move(unused));
        }
    "#;
    let file = Fixture::new(source);
    let dir = file.0.parent().unwrap();
    let root = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..");
    let tools = root.join("src/ripple-asm/target/release");
    let asm = dir.join("main.asm");
    let object = dir.join("main.pobj");
    let macros = dir.join("main.bfm");
    let bf = dir.join("main.bf");
    std::fs::write(&asm, rfl::compile_to_asm(source).unwrap()).unwrap();
    command(
        Command::new(tools.join("rasm"))
            .arg("assemble")
            .arg(&asm)
            .arg("-o")
            .arg(&object)
            .args(["--bank-size", "64000", "--max-immediate", "65535"]),
    );
    command(
        Command::new(tools.join("rlink"))
            .arg(root.join("runtime/crt0.pobj"))
            .arg(&object)
            .arg(root.join("runtime/libruntime.par"))
            .args([
                "-f",
                "macro",
                "--standalone",
                "--bf-host",
                "--memory-banks",
                "16",
                "--bank-size",
                "64000",
                "-o",
            ])
            .arg(&macros),
    );
    command(
        Command::new(root.join("src/bf-macro-expander/target/release/bfm"))
            .arg("expand")
            .arg(&macros)
            .args(["--strip-comments", "-o"])
            .arg(&bf),
    );
    let result = command(
        Command::new(root.join("target/release/rbf"))
            .arg(&bf)
            .args(["--jit", "--headless", "--timeout", "30", "--exit-code"]),
    );
    assert_eq!(String::from_utf8(result.stdout).unwrap(), "OK!");
}

#[test]
fn collections_and_repeated_environment_reuse_preserve_single_ownership() {
    parity(
        r#"
        fn make(n:i16)->once fn()->i16{let owner=owned(n,fn(n:&mut i16)=>{});once fn()=>*owner.borrow()}
        fn main(){
            let mut total=0;
            for(n in 0..128){let f=make(n);total+=f();let unused=make(n);drop(move(unused));}
            println(total);
            let mut callbacks:Vec<once fn()->i16> = [make(1),make(2)];
            match callbacks.pop(){Some(f)=>println(f()),None=>{}};
            callbacks.clear();
            let pair=(make(3),"tuple");let (f,text)=move(pair);println(f());println(text);
        }
    "#,
        "8128\n2\n3\ntuple\n",
    );
    reject(
        "fn main(){let values=[once fn()=>1];let copy=values;}",
        "cannot be copied",
    );
    reject(
        "fn main(){let values=[once fn()=>1];let f=values[0];}",
        "move-only element",
    );
}

#[test]
fn consuming_fp_laws_hold_for_option_and_fixed_error_result() {
    parity_file(
        r#"
        import {FunctorOnce,MonadOnce,map_once,flat_map_once,pure_once} from "std/fp";
        fn step(x:i16)->Option<i16>{Some(x+1)}
        fn next(x:i16)->Option<i16>{Some(x*2)}
        fn inject(x:i16)->Option<i16>{pure_once<Option,_>(x)}
        fn main(){
            println(map_once<Option,_,_>(Some(20),once fn(x:i16)=>x));
            println(flat_map_once<Option,_,_>(pure_once<Option,_>(20),step));
            println(flat_map_once<Option,_,_>(Some(20),inject));
            println(flat_map_once<Option,_,_>(flat_map_once<Option,_,_>(Some(20),step),next));
            println(flat_map_once<Option,_,_>(Some(20),once fn(x:i16)=>flat_map_once<Option,_,_>(step(x),next)));
            println(map_once<Result<_,String>,_,_>(Ok(21),once fn(x:i16)=>x*2));
            println(flat_map_once<Result<_,String>,i16,i16>(Err("stopped"),once fn(x:i16)=>{println("bad");Ok(x)}));
        }
    "#,
        "Some(20)\nSome(21)\nSome(20)\nSome(42)\nSome(42)\nOk(42)\nErr(stopped)\n",
    );
}

#[test]
fn generic_declarations_can_reference_named_cleanup_callbacks() {
    parity(
        r#"
        fn finish(value:&mut Option<once fn()->unit>)->unit {
            match __replace_value(value,None) {Some(f)=>f(),None=>{}}
        }
        fn guard<T>(value:T,finalizer:once fn()->unit) {
            let cleanup=owned(Some(move(finalizer)),finish);
        }
        fn main(){guard(42,once fn()=>println("released"));}
    "#,
        "released\n",
    );
}
