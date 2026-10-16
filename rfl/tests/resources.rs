//! Resource moves and deterministic cleanup must agree on native and VM targets.
use std::path::{Path, PathBuf};

struct Program(PathBuf);
impl Program {
    fn new(source: &str) -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let sequence = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let directory = std::env::temp_dir().join(format!(
            "rfl-resources-{}-{stamp}-{sequence}",
            std::process::id()
        ));
        std::fs::create_dir_all(&directory).unwrap();
        let path = directory.join("main.rfl");
        std::fs::write(&path, source).unwrap();
        Self(path)
    }
    fn path(&self) -> &Path {
        &self.0
    }
}
impl Drop for Program {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(self.0.parent().unwrap());
    }
}
fn parity(source: &str, status: i32, stdout: &str) {
    let program = Program::new(source);
    for result in [
        rfl::run_native_file(program.path(), false).unwrap_or_else(|e| panic!("{e}")),
        rfl::run_file(program.path()).unwrap_or_else(|e| panic!("{e}")),
    ] {
        assert_eq!(result.status, status, "{}", result.stdout);
        assert_eq!(result.stdout, stdout);
    }
}

#[test]
fn owners_move_borrow_and_finalize_once_at_lexical_exit() {
    parity(
        r#"
        fn consume(value:Owned<i16>) { println(*value.borrow()); }
        fn main()->i16 {
            let mut drops:i16=0;
            {
                let mut first=owned(7,fn(value:&mut i16)=>{ drops += 1; println(*value); });
                { let value=first.borrow_mut(); *value=42; }
                let second=move(first);
                consume(move(second));
                if (drops != 1) { return 1; }
            }
            if (drops != 1) { return 2; }
            42
        }
    "#,
        42,
        "42\n42\n",
    );
}

#[test]
fn implicit_resource_copies_are_rejected() {
    let source = "fn main() { let a=owned(1,fn(x:&mut i16)=>{}); let b=a; }";
    let error = rfl::check_source(source).unwrap_err();
    assert!(error.message.contains("move("), "{error}");
}

#[test]
fn cleanup_order_replacement_return_and_question_mark() {
    parity(
        r#"
        fn resource(n:i16)->Owned<i16> { owned(n,fn(n:&mut i16)=>{ println(*n); }) }
        fn early()->i16 {
            let a=resource(1);
            { let b=resource(2); let c=resource(3); return 7; };
            0
        }
        fn propagate()->Option<i16> {
            let a=resource(4); let b=resource(5);
            let empty:Option<i16> = None;
            let n=empty?;
            Some(n)
        }
        fn main()->i16 {
            if (early()!=7) {return 1;}
            propagate();
            {
                let mut a=resource(6);
                a=resource(7);
                let b=resource(8);
                drop(move(a));
                println(9);
            }
            42
        }
    "#,
        42,
        "3\n2\n1\n5\n4\n6\n7\n9\n8\n",
    );
}

#[test]
fn aggregates_destructure_match_wildcards_and_nested_finalizers() {
    parity(
        r#"
        struct Pair { first:Owned<i16>, second:Owned<i16> }
        type Tree = Empty | Leaf(Owned<i16>) | Branch(Tree,Tree);
        fn resource(n:i16)->Owned<i16> { owned(n,fn(n:&mut i16)=>{ println(*n); }) }
        fn main()->i16 {
            {
                let pair=Pair{first:resource(1),second:resource(2)};
                let Pair{first:a,second:_}=move(pair);
                println(*a.borrow());
            }
            {
                let tree=Branch(Leaf(resource(3)),Leaf(resource(4)));
                match move(tree) {
                    Branch(Leaf(previous),Empty)=>{return 1;},
                    Branch(Leaf(a),_)=>{println(*a.borrow());},
                    _=>{return 2;}
                }
                println(5);
            }
            {
                let inner=resource(6);
                let outer=owned(move(inner),fn(n:&mut Owned<i16>)=>{ println(7); println(*n.borrow()); });
            }
            42
        }
    "#,
        42,
        "2\n1\n1\n4\n3\n3\n5\n7\n6\n6\n",
    );
}

#[test]
fn vectors_arrays_and_reinitialization_in_loops() {
    parity(
        r#"
        fn resource(n:i16)->Owned<i16> { owned(n,fn(n:&mut i16)=>{ println(*n); }) }
        fn main()->i16 {
            {
                let mut values:Vec<Owned<i16>>=[];
                values.push(resource(1)); values.push(resource(2));
                values[0]=resource(3);
                match values.pop() { Some(value)=>{println(*value.borrow());}, None=>{return 1;} }
                println(4);
                values.clear();
                values.push(resource(5));
                let other=move(values);
            }
            {
                let array:[Owned<i16>;2]=[resource(6),resource(7)];
                let mut values=move(array).to_vec();
                values.clear();
            }
            let mut outer=resource(8);
            for (i in 0..3) {
                let value=resource(9);
                if (i==0) {continue;}
                if (i==1) {break;}
            }
            for (i in 0..2) { drop(move(outer)); outer=resource(10); }
            42
        }
    "#,
        42,
        "1\n2\n2\n4\n3\n5\n6\n7\n9\n9\n8\n10\n10\n",
    );
}

#[test]
fn move_diagnostics_cover_control_flow_borrows_and_implicit_aliases() {
    for (source, diagnostic) in [
        (
            "let a=resource(); drop(move(a)); drop(move(a));",
            "moved value",
        ),
        (
            "let a=resource(); let b=move(a); a.borrow();",
            "moved value",
        ),
        (
            "let a=resource(); let r=a.borrow(); drop(move(a)); println(*r);",
            "borrow",
        ),
        (
            "let a=resource(); if (true) { drop(move(a)); } a.borrow();",
            "moved value",
        ),
        (
            "let a=resource(); while (true) {drop(move(a));}",
            "repeating loop",
        ),
        (
            "let a=resource(); for (i in 0..3) {drop(move(a)); continue;}",
            "repeating loop",
        ),
        ("let a=resource(); let f=fn()=>{ a.borrow(); };", "capture"),
        ("let a=resource(); let r=a.borrow(); let b=*r;", ""),
        ("let a=resource(); let r=&a; let b=*r;", "borrowed resource"),
        ("let v=[resource()]; let s=v[0..1];", "owning slices"),
        ("let v=[resource()]; let x=v[0];", "move-only element"),
        ("let v=[resource()]; let x=v.get(0);", "get() cannot copy"),
        (
            "let a=resource(); match Some(1) {Some(x) if {drop(move(a));true}=>{}, _=>{}}",
            "guard cannot move",
        ),
    ] {
        let source = format!(
            "fn resource()->Owned<i16> {{owned(1,fn(x:&mut i16)=>{{}})}} fn main() {{ {source} }}"
        );
        if diagnostic.is_empty() {
            rfl::check_source(&source).unwrap();
            continue;
        }
        let error = rfl::check_source(&source).unwrap_err();
        assert!(
            error.message.contains(diagnostic),
            "expected {diagnostic}: {error}"
        );
    }
}

#[test]
fn owned_futures_cancel_unpolled_pending_and_nested_work_but_not_ready() {
    parity(
        r#"
        import { Future, Poll, Pending, Ready } from "std/future";
        import { Context } from "std/task";
        import { Reactor, block_on_with_budget } from "std/executor";
        struct Probe { done:bool, ready:bool }
        impl Future for Probe {
            type Output=i16;
            fn poll(&mut self,cx:&mut Context)->Poll<i16> {
                println(1);
                if (self.ready) {self.done=true; Ready(42)} else {Pending}
            }
            fn cancel(&mut self) { if (!self.done) {self.done=true; println(2);} }
        }
        fn probe(ready:bool)->Owned<Probe> {
            owned(Probe{done:false,ready:ready},fn(f:&mut Probe)=>{if (!f.done) {f.cancel();}})
        }
        struct Parent { child:Owned<Probe> }
        impl Future for Parent {
            type Output=i16;
            fn poll(&mut self,cx:&mut Context)->Poll<i16> {
                let child=self.child.borrow_mut(); child.poll(cx)
            }
            fn cancel(&mut self) { let child=self.child.borrow_mut(); child.cancel(); }
        }
        struct Clock { ticks:u32 }
        impl Reactor for Clock {
            fn now(&self)->u32 {self.ticks}
            fn turn(&mut self,wait:Option<u32>)->bool {self.ticks+=1;true}
        }
        fn main()->i16 {
            {let never=probe(false);}
            let mut clock=Clock{ticks:0};
            {
                let mut parent=owned(Parent{child:probe(false)},fn(f:&mut Parent)=>{f.cancel();});
                block_on_with_budget(parent.borrow_mut(),&mut clock,2);
            }
            {
                let mut done=probe(true);
                match block_on_with_budget(done.borrow_mut(),&mut clock,1) {Ok(42)=>{},_=>{return 1;}}
            }
            42
        }
    "#,
        42,
        "2\n1\n2\n1\n",
    );
}

#[test]
fn completed_resource_outputs_are_transferred_and_losing_outputs_are_cleaned() {
    parity(
        r#"
        import { Future, Poll, Pending, Ready, ready } from "std/future";
        import { Context } from "std/task";
        import { Reactor, block_on_with_budget, join_with_budget } from "std/executor";
        struct Maker { done:bool }
        impl Future for Maker {
            type Output=Owned<i16>;
            fn poll(&mut self,cx:&mut Context)->Poll<Owned<i16>> {
                self.done=true;
                Ready(owned(42,fn(x:&mut i16)=>{println(*x);}))
            }
            fn cancel(&mut self) {println(99);}
        }
        struct Never { unused:bool }
        impl Future for Never {
            type Output=String;
            fn poll(&mut self,cx:&mut Context)->Poll<String> {Pending}
            fn cancel(&mut self) {println(7);}
        }
        struct Clock { ticks:u32 }
        impl Reactor for Clock {
            fn now(&self)->u32 {self.ticks}
            fn turn(&mut self,wait:Option<u32>)->bool {self.ticks+=1;true}
        }
        fn main()->i16 {
            let mut clock=Clock{ticks:0};
            {
                let mut maker=Maker{done:false};
                match block_on_with_budget(&mut maker,&mut clock,1) {
                    Ok(value)=>{println(*value.borrow());},Err(_)=>{return 1;}
                }
            }
            println(1);
            {
                let mut maker=Maker{done:false};
                let mut text=ready("joined");
                match join_with_budget(&mut maker,&mut text,&mut clock,2) {
                    Ok((value,text))=>{println(text);println(*value.borrow());},
                    Err(_)=>{return 2;}
                }
            }
            {
                let mut maker=Maker{done:false};
                let mut never=Never{unused:false};
                join_with_budget(&mut maker,&mut never,&mut clock,2);
            }
            42
        }
    "#,
        42,
        "42\n42\n1\njoined\n42\n42\n7\n42\n",
    );
}

#[test]
fn branches_reinitialization_parameter_patterns_and_partial_initialization() {
    parity(
        r#"
        struct Pair { a:Owned<i16>, b:Owned<i16> }
        fn resource(n:i16)->Owned<i16> {owned(n,fn(n:&mut i16)=>{println(*n);})}
        fn consume(Pair{a:_,b:value}:Pair) {println(*value.borrow());}
        fn branch(flag:bool)->Owned<i16> {
            let a=resource(1);
            if (flag) {return move(a);}
            move(a)
        }
        fn partial()->i16 {
            let pair=Pair{a:resource(7),b:{return 42; resource(8)}};
            0
        }
        fn main()->i16 {
            {
                let a=branch(true); let b=branch(false);
            }
            consume(Pair{a:resource(2),b:resource(3)});
            let mut a=resource(4);
            if (true) {drop(move(a));a=resource(5);} else {drop(move(a));a=resource(6);}
            drop(move(a));
            partial()
        }
    "#,
        42,
        "1\n1\n2\n3\n3\n4\n5\n7\n",
    );
}

#[test]
fn managed_payloads_and_nested_callback_frames_reclaim_storage_repeatedly() {
    parity(
        r#"
        fn read(value:&mut String)->i16 { if (*value=="payload!") {1} else {1000} }
        fn main()->i16 {
            let mut count:i16=0;
            for (i in 0..150) {
                let mut data=owned("payload",fn(data:&mut String)=>{
                    count+=read(data);
                    let nested=owned(1,fn(x:&mut i16)=>{count+=*x;});
                    let padding:Vec<i16> = [1,2,3,4];
                    if (padding.len()!=4) {count=1000;}
                });
                {let value=data.borrow_mut(); *value=*value+"!";}
                if (i==0) {continue;}
            }
            if (count!=300) {return 1;}
            42
        }
    "#,
        42,
        "",
    );
}

#[test]
fn ownership_rejections_are_explicit_for_unsupported_sharing_and_partial_moves() {
    for (source,diagnostic) in [
        ("let a=resource(); let b=Pair{a:a};", "move-only resource cannot be copied"),
        ("let a=Pair{a:resource()}; let b=a.a;", "resource field"),
        ("let a=Pair{a:resource()}; let b=move(a.a);", "move() needs a local name"),
        ("let a=Pair{a:resource()}; drop(move(a)); a.a.borrow();", "moved value"),
        ("let a=resource(); accept(a);", "move-only resource cannot be copied"),
        ("let mut v=[resource()]; v.push({drop(move(v));resource()});", "moved value"),
        ("let mut v=[resource()]; v[0]={drop(move(v));resource()};", "moved value"),
        ("let mut p=Pair{a:resource()}; p.a={drop(move(p));resource()};", "moved value"),
        ("let a=resource(); let b=move(a); let f=fn()=>{a.borrow();};", "capture"),
        ("let a=resource(); let f=fn()=>{drop(move(a));};", "closure capture"),
        ("let a:Option<Owned<i16>> = Some(resource()); match move(a) {Some(x) if true=>{},_=>{}}", "guarded resource matches"),
        ("let a=Pair{a:resource()}; a.inspect(); let b=a;", "move-only resource cannot be copied"),
        ("Pair{a:resource()}.inspect();", "resource temporary"),
        ("let mut a=resource(); let b=a.borrow_mut(); let c=a.borrow();", "borrow"),
        ("let mut a=resource(); let b=a.borrow(); a=resource();", "borrow"),
        ("let a=resource(); let b=&a; let c=move(b);", "cannot move a borrowed reference"),
    ] {
        let source=format!("struct Pair {{a:Owned<i16>}} impl Pair {{fn inspect(&self) {{}}}} fn resource()->Owned<i16> {{owned(1,fn(x:&mut i16)=>{{}})}} fn accept(a:Owned<i16>) {{}} fn main() {{ {source} }}");
        let error=rfl::check_source(&source).unwrap_err();
        assert!(error.message.contains(diagnostic),"expected {diagnostic}: {error}");
    }
}

#[test]
fn unused_generic_declarations_check_explicit_owners_and_specializations_check_t() {
    for source in [
        "fn bad<T>(x:Owned<T>)->Owned<T> {x} fn main() {}",
        "fn bad<T>(x:Owned<T>) {let f=fn()=>{x.borrow();};} fn main() {}",
        "fn copy<T>(x:T)->T {x} fn main() {let x=owned(1,fn(x:&mut i16)=>{}); let y=copy(move(x));}",
    ] {assert!(rfl::check_source(source).is_err(),"{source}");}
    rfl::check_source("fn transfer<T>(x:T)->T {move(x)} fn main() {let x=owned(1,fn(x:&mut i16)=>{}); let y=transfer(move(x));}").unwrap();
}

#[test]
fn owner_finalization_runs_on_the_brainfuck_cpu() {
    use std::process::Command;
    fn command(cmd: &mut Command) -> std::process::Output {
        let result = cmd.output().unwrap_or_else(|e| panic!("{cmd:?}: {e}"));
        assert!(
            result.status.success(),
            "{cmd:?}: {}",
            String::from_utf8_lossy(&result.stderr)
        );
        result
    }
    let source="fn cleanup(x:&mut i16) {putc(*x);} fn main() {let mut first=owned(88,cleanup); let old=__replace_value(&mut first,owned(89,cleanup)); move(old).into_inner(); let second=move(first); let erased=__erase_owned(move(second)); drop(move(erased));}";
    let program = Program::new(source);
    let dir = program.path().parent().unwrap();
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..");
    let tools = root.join("src/ripple-asm/target/release");
    let asm = dir.join("main.asm");
    let obj = dir.join("main.pobj");
    let macros = dir.join("main.bfm");
    let bf = dir.join("main.bf");
    std::fs::write(&asm, rfl::compile_to_asm(source).unwrap()).unwrap();
    command(
        Command::new(tools.join("rasm"))
            .arg("assemble")
            .arg(&asm)
            .arg("-o")
            .arg(&obj)
            .args(["--bank-size", "64000", "--max-immediate", "65535"]),
    );
    command(
        Command::new(tools.join("rlink"))
            .arg(root.join("runtime/crt0.pobj"))
            .arg(&obj)
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
    assert_eq!(String::from_utf8(result.stdout).unwrap(), "Y");
}

#[test]
fn question_mark_transfers_resource_payloads_on_both_paths() {
    parity(
        r#"
        fn resource(n:i16)->Owned<i16> {owned(n,fn(n:&mut i16)=>{println(*n);})}
        fn extract(ok:bool)->Result<Owned<i16>,Owned<i16>> {
            let guard=resource(1);
            let result:Result<Owned<i16>,Owned<i16>> = if (ok) {Ok(resource(2))} else {Err(resource(3))};
            let value=move(result)?;
            Ok(move(value))
        }
        fn main()->i16 {
            match extract(true) {Ok(value)=>{println(*value.borrow());},Err(_)=>{return 1;}}
            match extract(false) {Err(value)=>{println(*value.borrow());},Ok(_)=>{return 2;}}
            42
        }
    "#,
        42,
        "1\n2\n2\n1\n3\n3\n",
    );
}

#[test]
fn indirect_calls_empty_payloads_and_resource_vector_growth() {
    parity(
        r#"
        fn main()->i16 {
            let mut drops:i16=0;
            let consume=fn(value:Owned<String>)=>{println(*value.borrow());};
            let text=owned("hello",fn(value:&mut String)=>{drops+=1;});
            consume(move(text));
            if (drops!=1) {return 1;}
            owned({},fn(value:&mut unit)=>{drops+=1;});
            if (drops!=2) {return 2;}
            let mut values:Vec<Owned<i16>> = [];
            for (i in 0..50) {values.push(owned(i,fn(value:&mut i16)=>{drops+=1;}));}
            if (drops!=2 || values.len()!=50) {return 3;}
            values.pop();
            if (drops!=3) {return 6;}
            for (i in 0..24) {values.pop();}
            if (drops!=27) {return 4;}
            values.clear();
            if (drops!=52) {return 5;}
            42
        }
    "#,
        42,
        "hello\n",
    );
}
