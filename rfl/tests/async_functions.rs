//! Compiler-owned lazy functions share the ordinary Future contract.
use std::path::{Path, PathBuf};

struct Program(PathBuf);
impl Program {
    fn new(source: &str) -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let dir = std::env::temp_dir().join(format!(
            "rfl-async-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("main.rfl");
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
fn parity(source: &str, stdout: &str) {
    let program = Program::new(source);
    for run in [
        rfl::run_native_file(program.path(), false),
        rfl::run_file(program.path()),
    ] {
        let result = run.unwrap_or_else(|e| panic!("{e}"));
        assert_eq!(result.status, 42, "{}", result.stdout);
        assert_eq!(result.stdout, stdout);
    }
}

#[test]
fn constructors_are_lazy_and_function_values_use_the_same_protocol() {
    parity(
        r#"
        import { Reactor, block_on } from "std/executor";
        struct Clock { time:u32 }
        impl Reactor for Clock {
            fn now(&self)->u32 {self.time}
            fn turn(&mut self,wait:Option<u32>)->bool {true}
        }
        async fn work(x:i16)->i16 { println("body"); x+2 }
        fn main()->i16 {
            let call:fn(i16)->Async<i16> = work;
            let mut future=call(40);
            println("created");
            let mut r=Clock {time:0};
            match block_on(&mut future,&mut r) {Ok(value)=>value,_=>1}
        }
    "#,
        "created\nbody\n",
    );
}

#[test]
fn unpolled_frames_drop_arguments_in_reverse_order_and_cancel_is_idempotent() {
    parity(
        r#"
        import { Future } from "std/future";
        async fn unused(a:Owned<i16>,b:Owned<i16>) { println("unexpected body"); }
        fn value(n:i16)->Owned<i16> {
            println(n);
            owned(n,fn(n:&mut i16)=>{println(*n+10);})
        }
        fn main()->i16 {
            let mut first=unused(value(1),value(2));
            let second=unused(value(3),value(4));
            first.cancel(); first.cancel();
            drop(move(second));
            drop(move(first));
            42
        }
    "#,
        "1\n2\n3\n4\n12\n11\n14\n13\n",
    );
}

#[test]
fn generic_bodies_return_managed_and_move_only_values_and_question_mark() {
    parity(
        r#"
        import { Reactor, block_on } from "std/executor";
        struct Clock { time:u32 }
        impl Reactor for Clock {
            fn now(&self)->u32 {0}
            fn turn(&mut self,wait:Option<u32>)->bool {true}
        }
        async fn echo<T>(value:T)->T {move(value)}
        fn fail()->Result<i16,String> {Err("failure")}
        async fn early()->Result<i16,String> {let x=fail()?;Ok(x)}
        fn main()->i16 {
            let mut r=Clock {time:0};
            let mut text=echo("hello");
            match block_on(&mut text,&mut r) {Ok(t)=>println(t),_=>{return 1;}}
            let mut owner=echo(owned(42,fn(n:&mut i16)=>{println(*n);}));
            let owner=match block_on(&mut owner,&mut r) {Ok(n)=>move(n),_=>{return 2;}};
            println("returned");drop(move(owner));
            let mut early=early();
            match block_on(&mut early,&mut r) {Ok(Err(e))=>println(e),_=>{return 3;}}
            42
        }
    "#,
        "hello\nreturned\n42\nfailure\n",
    );
}

#[test]
fn future_handles_cannot_copy_and_borrowed_parameters_cannot_escape_into_frames() {
    for (source, message) in [
        (
            "async fn f()->i16 {42} fn main(){let a=f();let b=a;}",
            "move-only resource cannot be copied",
        ),
        (
            "async fn f(x:&i16)->i16 {*x} fn main(){}",
            "borrowed references",
        ),
        ("async fn main(){}", "async functions cannot be main"),
        (
            "export extern \"C\" async fn f()->i16 {42} fn main(){}",
            "async functions cannot be main or exported",
        ),
    ] {
        let error = rfl::check_source(source).unwrap_err();
        assert!(error.to_string().contains(message), "{error}");
    }
}

#[test]
fn await_preserves_loops_branches_temporaries_and_a_fresh_poll_context() {
    parity(
        r#"
        import { Reactor, block_on } from "std/executor";
        import { Future, yield_now, sleep, ready } from "std/future";
        struct Clock {time:u32}
        impl Reactor for Clock {
            fn now(&self)->u32 {self.time}
            fn turn(&mut self,wait:Option<u32>)->bool {
                match wait {Some(t)=>{self.time+=t;},None=>{self.time+=1;}}true
            }
        }
        async fn child(value:i16)->i16 {yield_now().await;value}
        async fn work()->i16 {
            let mut text="start";
            let mut total:i16=0;
            for (n in [1,2,3]) {
                text=text+"!";
                let value=if (n==2) {child(20).await} else {ready(n).await};
                total+=value;
                if (n==1) {continue;}
                yield_now().await;
            }
            let timer=match sleep(7) {Ok(t)=>t,Err(_)=>{return 1;}};
            timer.await;
            let mut n:i16=0;
            while (n<3) {n+=1;yield_now().await;if (n==2) {break;}}
            if (text!="start!!!" || n!=2) {return 2;}
            // A snapshot made before awaiting an argument must survive too.
            total+child(18).await
        }
        fn main()->i16 {
            let mut r=Clock{time:100};
            let mut work=work();
            match block_on(&mut work,&mut r) {Ok(value)=>{if (r.time<107) {return 3;}value},_=>4}
        }
    "#,
        "",
    );
}

#[test]
fn suspension_rejects_live_loans_including_earlier_call_arguments() {
    for body in [
        "let n:i16=1;let p=&n;yield_now().await;println(*p);",
        "let n:i16=1;take(&n,yield_now().await);",
        "let call=fn()=>{yield_now().await;};",
    ] {
        let source = format!(
            r#"
            import {{ Future, yield_now }} from "std/future";
            fn take(n:&i16,value:unit) {{}}
            async fn work() {{ {body} }}
            fn main() {{}}
        "#
        );
        let program = Program::new(&source);
        let error = rfl::check_file(program.path()).unwrap_err();
        let text = error.to_string();
        assert!(
            text.contains("cannot cross await") || text.contains("only allowed inside an async"),
            "{error}"
        );
    }
    let program = Program::new(
        r#"
        import { Future, yield_now } from "std/future";
        async fn work() {let n=1;{let p=&n;println(*p);}yield_now().await;}
        fn main() {}
    "#,
    );
    rfl::check_file(program.path()).unwrap();
}

#[test]
fn cancellation_at_await_drops_the_child_then_live_locals_once() {
    parity(
        r#"
        import { Future, Poll, Pending } from "std/future";
        import { Context, task_set } from "std/task";
        struct Probe {token:Owned<i16>}
        impl Probe {fn cancel(&mut self) {println("wrong inherent cancel");}}
        impl Future for Probe {
            type Output=unit;
            fn poll(&mut self,cx:&mut Context)->Poll<unit> {cx.waker.wake_at(cx.now+5);Pending}
            fn cancel(&mut self) {println("child cancel");}
        }
        fn token(n:i16)->Owned<i16> {owned(n,fn(n:&mut i16)=>{println(*n);})}
        async fn work(a:Owned<i16>) {
            let b=token(2);
            Probe{token:token(3)}.await;
            println("unexpected completion");
        }
        fn main()->i16 {
            let q=match task_set(1,0) {Ok(q)=>q,_=>{return 1;}};
            let w=match q.allocate() {Ok(w)=>w,_=>{return 2;}};
            let mut cx=Context{waker:w,now:0};
            let mut f=work(token(1));
            match f.poll(&mut cx) {Pending=>{},_=>{return 3;}}
            f.cancel();f.cancel();drop(move(f));
            let unpolled=work(token(4));drop(move(unpolled));
            42
        }
    "#,
        "child cancel\n3\n2\n1\n4\n",
    );
}

#[test]
fn independent_frames_resume_out_of_order_and_await_generic_associated_outputs() {
    parity(
        r#"
        import { Future, Poll, Pending, Ready, yield_now } from "std/future";
        import { Context, task_set } from "std/task";
        async fn work(value:Owned<i16>)->Owned<i16> {yield_now().await;move(value)}
        async fn forward<F:Future>(future:F)->F::Output {move(future).await}
        fn token(n:i16)->Owned<i16> {owned(n,fn(n:&mut i16)=>{println(*n);})}
        fn main()->i16 {
            let q=match task_set(1,0) {Ok(q)=>q,_=>{return 1;}};
            let w=match q.allocate() {Ok(w)=>w,_=>{return 2;}};
            let mut cx=Context{waker:w,now:0};
            let mut first=forward(work(token(1)));
            let mut second=forward(work(token(2)));
            match first.poll(&mut cx) {Pending=>{},_=>{return 3;}}
            match second.poll(&mut cx) {Pending=>{},_=>{return 4;}}
            let result=match first.poll(&mut cx) {Ready(value)=>move(value),_=>{return 5;}};
            println("first ready");
            drop(move(second));
            drop(move(first));
            println("output alive");
            drop(move(result));
            42
        }
    "#,
        "first ready\n2\noutput alive\n1\n",
    );
}

#[test]
fn pending_partial_arguments_and_early_error_unwind_only_initialized_values() {
    parity(
        r#"
        import { Reactor, block_on_with_budget } from "std/executor";
        import { Future, Pending, yield_now } from "std/future";
        import { Context, task_set } from "std/task";
        struct Clock {time:u32}
        impl Reactor for Clock {
            fn now(&self)->u32 {0}
            fn turn(&mut self,wait:Option<u32>)->bool {true}
        }
        fn token(n:i16)->Owned<i16> {owned(n,fn(n:&mut i16)=>{println(*n);})}
        fn combine(a:Owned<i16>,b:Owned<i16>)->Owned<i16> {move(b)}
        async fn later()->Owned<i16> {yield_now().await;token(2)}
        async fn partial()->Owned<i16> {combine(token(1),later().await)}
        async fn failure()->Result<i16,String> {yield_now().await;Err("expected")}
        async fn early()->Result<i16,String> {let t=token(3);let n=failure().await?;Ok(n)}
        fn main()->i16 {
            let q=match task_set(1,0) {Ok(q)=>q,_=>{return 1;}};
            let w=match q.allocate() {Ok(w)=>w,_=>{return 2;}};
            let mut cx=Context{waker:w,now:0};
            let mut f=partial();
            match f.poll(&mut cx) {Pending=>{},_=>{return 3;}}
            drop(move(f));
            let mut r=Clock{time:0};
            let mut f=early();
            match block_on_with_budget(&mut f,&mut r,5) {Ok(Err(text))=>println(text),_=>{return 4;}}
            f.cancel();f.cancel();
            42
        }
    "#,
        "1\n3\nexpected\n",
    );
}

#[test]
fn polling_completed_or_cancelled_frames_reports_a_protocol_error() {
    let template = r#"
        import { Future, Ready } from "std/future";
        import { Context, task_set } from "std/task";
        async fn work()->i16 {println("ran");42}
        fn main()->i16 {
            let q=match task_set(1,0) {Ok(q)=>q,_=>{return 2;}};
            let w=match q.allocate() {Ok(w)=>w,_=>{return 3;}};
            let mut cx=Context{waker:w,now:0};
            let mut f=work();
            $ACTION
            f.poll(&mut cx);
            println("unexpected repeat");
            42
        }
    "#;
    for (action, prefix) in [
        ("f.cancel();f.cancel();", ""),
        (
            "match f.poll(&mut cx) {Ready(42)=>{},_=>{return 4;}}",
            "ran\n",
        ),
    ] {
        let program = Program::new(&template.replace("$ACTION", action));
        for run in [
            rfl::run_native_file(program.path(), false),
            rfl::run_file(program.path()),
        ] {
            let run = run.unwrap();
            assert_eq!(run.status, 1);
            assert_eq!(
                run.stdout,
                format!("{prefix}error: future polled after completion or cancellation\n")
            );
        }
    }
}

#[test]
fn deep_recursive_await_uses_the_frame_driver_instead_of_the_hardware_stack() {
    let source = r#"
        import { Future, Pending, Ready, yield_now } from "std/future";
        import { Context, task_set } from "std/task";
        async fn count(n:u16)->u16 {
            if (n==0) {yield_now().await;0}
            else {count(n-1).await+1}
        }
        fn main()->i16 {
            let q=match task_set(1,0) {Ok(q)=>q,_=>{return 1;}};
            let w=match q.allocate() {Ok(w)=>w,_=>{return 2;}};
            let mut cx=Context{waker:w,now:0};
            let mut f=count($DEPTH);
            match f.poll(&mut cx) {Pending=>{},_=>{return 3;}}
            match f.poll(&mut cx) {Ready($DEPTH)=>{},_=>{return 4;}}
            f.cancel();
            42
        }
    "#;
    for (depth, native) in [(6000, true), (512, false)] {
        let program = Program::new(&source.replace("$DEPTH", &depth.to_string()));
        let run = if native {
            rfl::run_native_file(program.path(), false)
        } else {
            rfl::run_file(program.path())
        }
        .unwrap();
        assert_eq!(run.status, 42, "{}", run.stdout);
        assert_eq!(run.stdout, "");
    }
}
