//! Library effects share the ordinary lazy future and ownership protocols.
use std::path::PathBuf;
struct Program(PathBuf);
impl Program {
    fn new(source: &str) -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let dir = std::env::temp_dir().join(format!(
            "rfl-effect-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("main.rfl");
        std::fs::write(&path, format!("{PRELUDE}\n{source}")).unwrap();
        Self(path)
    }
}
impl Drop for Program {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(self.0.parent().unwrap());
    }
}
const PRELUDE: &str = r#"
import {Result} from "std/result";
import {OwnedEffect as Effect,run,attempt,access,succeed,fail,map,flat_map,map_error,catch_all,provide,from_async,from_future,ensuring,scoped} from "std/effect_once";
import {Environment,with_environment,own_environment} from "std/effect_env";
import {Future,Poll,Pending,Ready,yield_now} from "std/future";
import {Reactor,block_on,block_on_with_budget} from "std/executor";
import {Context,task_set} from "std/task";
struct Clock {time:u32}
impl Reactor for Clock {
 fn now(&self)->u32{self.time}
 fn turn(&mut self,wait:Option<u32>)->bool{self.time+=1;true}
}
fn execute<R,E,A>(effect:Effect<R,E,A>,value:R)->Result<A,E> {
 let mut f=run(move(effect),move(value));let mut r=Clock{time:0};
 block_on(&mut f,&mut r).unwrap()
}
fn token(n:i16)->Owned<i16>{owned(n,fn(n:&mut i16)=>{println(*n);})}
"#;
fn assert_runs(source: &str, expected: &str, status: i32) {
    let program = Program::new(source);
    for native in [true, false] {
        let run = if native {
            rfl::run_native_file(&program.0, false)
        } else {
            rfl::run_file(&program.0)
        }
        .unwrap_or_else(|e| panic!("{e}"));
        assert_eq!(run.status, status, "native={native}: {}", run.stdout);
        assert_eq!(run.stdout, expected, "native={native}");
    }
}
#[test]
fn construction_composition_and_root_future_are_lazy() {
    assert_runs(
        r#"
        struct Services {message:String,count:i16}
        async fn later(n:i16)->Result<i16,String>{yield_now().await;println("async");Ok(n)}
        fn main(){
            let read:Effect<Services,String,i16> = access(once fn(s:&mut Services)=>{s.count+=1;println(s.message);s.count});
            let work=flat_map(move(read),once fn(n:i16)=>from_async(once fn(env:Environment<Services>)=>later(n+41)));
            let work=map(move(work),once fn(n:i16)=>{println("map");n});
            let mut f=run(move(work),Services{message:"read",count:0});
            println("constructed");let mut r=Clock{time:0};println(block_on(&mut f,&mut r).unwrap());
        }
    "#,
        "constructed\nread\nasync\nmap\nOk(42)\n",
        0,
    );
}
#[test]
fn failures_short_circuit_map_errors_and_recover_lazily() {
    assert_runs(
        r#"
        fn main(){
            let work:Effect<unit,String,i16> = fail("bad");
            let work=map(move(work),once fn(n:i16)=>{println("unexpected map");n});
            let work:Effect<unit,String,i16> = flat_map(move(work),once fn(n:i16)=>{println("unexpected bind");succeed(n)});
            let work=map_error(move(work),once fn(e:String)=>{println(e);42});
            let work:Effect<unit,String,i16> = catch_all(move(work),once fn(e:i16)=>{println("recover");succeed(e)});
            println("constructed");println(execute(move(work),{}));
            let work:Effect<unit,String,i16> = succeed(7);
            let work:Effect<unit,i16,i16> = catch_all(move(work),once fn(e:String)=>{println("unexpected recovery");fail(9)});
            println(execute(move(work),{}));
        }
    "#,
        "constructed\nbad\nrecover\nOk(42)\nOk(7)\n",
        0,
    );
}
#[test]
fn environments_preserve_owned_state_and_provide_replaces_it_for_a_subprogram() {
    assert_runs(
        r#"
        struct Services {counter:Owned<i16>}
        fn increment()->Effect<Services,String,i16>{access(once fn(s:&mut Services)=>{let n=s.counter.borrow_mut();*n+=1;*n})}
        async fn twice(env:Environment<Services>)->Result<i16,String>{
            with_environment(env,once fn(s:&mut Services)=>{let n=s.counter.borrow_mut();*n+=1;});
            yield_now().await;
            Ok(with_environment(env,once fn(s:&mut Services)=>*s.counter.borrow()))
        }
        fn main(){
            let work=flat_map(increment(),once fn(n:i16)=>{println(n);from_async(once fn(env:Environment<Services>)=>twice(env))});
            println(execute(move(work),Services{counter:token(40)}));
            let work:Effect<unit,String,i16> = provide(increment(),Services{counter:token(10)});
            println(execute(move(work),{}));
        }
    "#,
        "41\n42\nOk(42)\n11\nOk(11)\n",
        0,
    );
}
#[test]
fn outputs_errors_and_skipped_continuations_keep_one_owner() {
    assert_runs(
        r#"
        import {ready_owned} from "std/future_owned";
        fn main(){
            let value=token(1);let work:Effect<unit,String,Owned<i16>> = succeed(move(value));
            let value=execute(move(work),{}).unwrap();println("output alive");drop(move(value));
            let error=token(2);let unused=token(3);
            let work:Effect<unit,Owned<i16>,i16> = fail(move(error));
            let work:Effect<unit,Owned<i16>,i16> = flat_map(move(work),once fn(n:i16)=>{unused.borrow();succeed(n)});
            match execute(move(work),{}) {Err(error)=>{println("error alive");drop(move(error));},Ok(_)=>{}}
            let captured=token(4);let unused:Effect<unit,String,unit> = attempt(once fn(env:Environment<unit>)=>{captured.borrow();println("unexpected");Ok({})});
            drop(move(unused));
            let work:Effect<unit,String,Owned<i16>> = from_future(once fn(env:Environment<unit>)=>ready_owned(token(6)));
            let value=execute(move(work),{}).unwrap();println("future output alive");drop(move(value));
        }
    "#,
        "output alive\n1\n3\nerror alive\n2\n4\nfuture output alive\n6\n",
        0,
    );
}
#[test]
fn finalizers_register_on_poll_and_nest_in_reverse_order() {
    assert_runs(
        r#"
        fn main(){
            let work:Effect<unit,String,i16> = succeed(42);
            let work=ensuring(ensuring(move(work),once fn()=>println("inner")),once fn()=>println("outer"));
            println(execute(move(work),{}));
            let work:Effect<unit,String,i16> = fail("typed");
            println(execute(ensuring(move(work),once fn()=>println("failure cleanup")),{}));
            let capture=token(5);let work:Effect<unit,String,i16> = succeed(0);
            let unstarted=ensuring(move(work),once fn()=>{capture.borrow();println("unexpected finalizer");});
            drop(move(unstarted));
            let work:Effect<unit,String,i16> = succeed(0);
            let unpolled=run(ensuring(move(work),once fn()=>println("unexpected unpolled")),{});
            drop(move(unpolled));
        }
    "#,
        "inner\nouter\nOk(42)\nfailure cleanup\nErr(typed)\n5\n",
        0,
    );
}
#[test]
fn scoped_resources_release_on_success_failure_and_pending_cancellation() {
    assert_runs(r#"
        struct Never {value:Owned<i16>}
        impl Future for Never {
            type Output=i16;
            fn poll(&mut self,cx:&mut Context)->Poll<i16>{cx.waker.wake();Pending}
            fn cancel(&mut self){println("cancel child");}
        }
        fn acquire(n:i16)->Effect<unit,String,Owned<i16>>{attempt(once fn(env:Environment<unit>)=>{println("acquire");Ok(token(n))})}
        fn release(value:&mut Owned<i16>)->unit{println("release");}
        fn main()->i16{
            let work=scoped(acquire(1),once fn(resource:Environment<Owned<i16>>)=>access<unit,String,i16>(once fn(env:&mut unit)=>with_environment(resource,once fn(value:&mut Owned<i16>)=>*value.borrow())),release);
            println(execute(move(work),{}));
            let work=scoped(acquire(2),once fn(resource:Environment<Owned<i16>>)=>fail<unit,String,i16>("use failed"),release);
            println(execute(move(work),{}));
            let work=scoped(fail<unit,String,Owned<i16>>("acquire failed"),once fn(resource:Environment<Owned<i16>>)=>{println("unexpected use");succeed<unit,String,i16>(0)},release);
            println(execute(move(work),{}));
            let work=scoped(acquire(3),once fn(resource:Environment<Owned<i16>>)=>from_future<unit,String,Never>(once fn(env:Environment<unit>)=>Never{value:token(4)}),release);
            let work=ensuring(move(work),once fn()=>println("finally"));
            let mut f=run(move(work),{});
            let q=task_set(1,0).unwrap();let w=q.allocate().unwrap();let mut cx=Context{waker:w,now:0};
            match f.poll(&mut cx){Pending=>{},_=>{return 1;}}
            f.cancel();f.cancel();drop(move(f));0
        }
    "#,"acquire\nrelease\n1\nOk(1)\nacquire\nrelease\n2\nErr(use failed)\nErr(acquire failed)\nacquire\ncancel child\n4\nrelease\n3\nfinally\n",0);
}
#[test]
fn generic_consuming_fp_contracts_work_for_effects() {
    assert_runs(
        r#"
        import {FunctorOnce,MonadOnce,map_once,flat_map_once,pure_once} from "std/fp";
        fn step(n:i16)->Effect<unit,String,i16>{succeed(n+1)}
        fn double(n:i16)->Effect<unit,String,i16>{succeed(n*2)}
        fn inject(n:i16)->Effect<unit,String,i16>{pure_once<Effect<unit,String,_>,_>(n)}
        fn main(){
            println(execute(map_once<Effect<unit,String,_>,_,_>(succeed<unit,String,i16>(20),once fn(n:i16)=>n),{}));
            println(execute(flat_map_once<Effect<unit,String,_>,_,_>(pure_once<Effect<unit,String,_>,_>(20),step),{}));
            println(execute(flat_map_once<Effect<unit,String,_>,_,_>(succeed<unit,String,i16>(20),inject),{}));
            let left=flat_map(flat_map(succeed<unit,String,i16>(20),step),double);
            let right=flat_map(succeed<unit,String,i16>(20),once fn(n:i16)=>flat_map(step(n),double));
            println(execute(move(left),{}));println(execute(move(right),{}));
        }
    "#,
        "Ok(20)\nOk(21)\nOk(20)\nOk(42)\nOk(42)\n",
        0,
    );
}
#[test]
fn checked_environment_views_reject_stale_and_reentrant_access() {
    for action in [
        "drop(move(owner));with_environment(env,once fn(n:&mut i16)=>{println(\"unexpected\");});",
        "let alias=env;with_environment(env,once fn(n:&mut i16)=>with_environment(alias,once fn(m:&mut i16)=>{println(\"unexpected\");}));",
    ] {
        assert_runs(&format!(r#"fn main(){{let (owner,env)=own_environment(42);{action}}}"#),"error: effect environment is unavailable or already borrowed\n",1);
    }
}
#[test]
fn malformed_environment_providers_cannot_return_uninitialized_results() {
    assert_runs(
        r#"
        fn main(){let env:Environment<i16> = Environment(fn(visitor:once fn(&mut i16)->unit)=>{});with_environment(env,once fn(n:&mut i16)=>*n);println("unexpected");}
    "#,
        "error: effect environment provider did not invoke its visitor\n",
        1,
    );
}
#[test]
fn effect_descriptions_and_environments_reject_implicit_resource_copies() {
    for body in [
        "let p:Effect<unit,unit,i16> = succeed(1);let copy=p;",
        "let p:Effect<unit,unit,i16> = succeed(1);let f=run(move(p),{});run(move(p),{});",
        "let p:Effect<unit,unit,i16> = succeed(1);let f=fn()=>run(move(p),{});",
    ] {
        let program = Program::new(&format!("fn main(){{{body}}}"));
        let error = rfl::check_file(&program.0).unwrap_err();
        assert!(
            error.message.contains("copied")
                || error.message.contains("moved")
                || error.message.contains("cannot move a closure capture"),
            "{error}"
        );
    }
}

#[test]
fn effect_roots_use_task_scope_scheduling_and_budget_cancellation() {
    assert_runs(
        r#"
        import {task_scope} from "std/scope";
        struct Never {value:Owned<i16>}
        impl Future for Never {
            type Output=i16;
            fn poll(&mut self,cx:&mut Context)->Poll<i16>{cx.waker.wake();Pending}
            fn cancel(&mut self){println("budget cancel");}
        }
        async fn later(n:i16)->Result<i16,String>{yield_now().await;Ok(n)}
        fn main()->i16{
            let mut scope=task_scope(2,0).unwrap();
            let one:Effect<unit,String,i16> = from_async(once fn(env:Environment<unit>)=>later(1));
            let two:Effect<unit,String,i16> = from_async(once fn(env:Environment<unit>)=>later(2));
            let mut a=scope.spawn(run(move(one),{})).unwrap();let mut b=scope.spawn(run(move(two),{})).unwrap();
            let mut clock=Clock{time:0};match scope.run_with_budget(&mut clock,8){Ok(_)=>{},_=>{return 1;}}
            println(block_on(&mut a,&mut clock).unwrap().unwrap());println(block_on(&mut b,&mut clock).unwrap().unwrap());
            let work:Effect<unit,String,i16> = from_future(once fn(env:Environment<unit>)=>Never{value:token(3)});
            let work:Effect<unit,String,i16> = catch_all(move(work),once fn(error:String)=>{println("unexpected recovery");succeed(0)});
            let mut f=run(ensuring(move(work),once fn()=>println("budget finalizer")),{});
            match block_on_with_budget(&mut f,&mut clock,2){Err(_)=>{},_=>{return 2;}}0
        }
    "#,
        "Ok(1)\nOk(2)\nbudget cancel\n3\nbudget finalizer\n",
        0,
    );
}

#[test]
fn long_chains_execute_drop_and_cancel_on_both_targets() {
    let template = r#"
        fn chain(n:u16,pending:bool)->Effect<unit,unit,u16>{
            let mut p:Effect<unit,unit,u16> = if(pending){from_async(once fn(env:Environment<unit>)=>later())}else{succeed(0)};
            for(i in 0..n){p=flat_map(move(p),once fn(x:u16)=>succeed<unit,unit,u16>(x+1));}
            move(p)
        }
        async fn later()->Result<u16,unit>{yield_now().await;Ok(0)}
        fn main()->i16{
            let q=task_set(1,0).unwrap();let w=q.allocate().unwrap();let mut cx=Context{waker:w,now:0};
            let unused=chain($DEPTH,false);drop(move(unused));println("dropped");
            let mut f=run(chain($DEPTH,false),{});
            match f.poll(&mut cx){Ready(Ok($DEPTH))=>{},_=>{return 1;}}println("ran");
            let mut f=run(chain($DEPTH,true),{});
            match f.poll(&mut cx){Pending=>{},_=>{return 2;}}
            f.cancel();f.cancel();println("cancelled");0
        }
    "#;
    for (depth, native) in [(6000, true), (512, false)] {
        let p = Program::new(&template.replace("$DEPTH", &depth.to_string()));
        let run = if native {
            rfl::run_native_file(&p.0, false)
        } else {
            rfl::run_file(&p.0)
        }
        .unwrap_or_else(|e| panic!("{e}"));
        assert_eq!(run.status, 0, "native={native}: {}", run.stdout);
        assert_eq!(run.stdout, "dropped\nran\ncancelled\n");
    }
}

#[test]
fn escaping_run_and_resource_views_expire_and_protocol_faults_are_not_recovered() {
    for body in [
        r#"
            let p:Effect<i16,String,Environment<i16>> = attempt(once fn(env:Environment<i16>)=>Ok(env));
            let view=execute(move(p),42).unwrap();
            let p:Effect<unit,String,i16> = attempt(once fn(env:Environment<unit>)=>Ok(with_environment(view,once fn(n:&mut i16)=>*n)));
            let p:Effect<unit,String,i16> = catch_all(move(p),once fn(e:String)=>{println("unexpected recovery");succeed(0)});
            execute(move(p),{});
        "#,
        r#"
            let acquire:Effect<unit,String,i16> = succeed(42);
            let p=scoped(move(acquire),once fn(view:Environment<i16>)=>succeed<unit,String,Environment<i16>>(view),fn(n:&mut i16)=>{});
            let view=execute(move(p),{}).unwrap();
            with_environment(view,once fn(n:&mut i16)=>*n);
        "#,
    ] {
        assert_runs(
            &format!("fn main(){{{body}}}"),
            "error: effect environment is unavailable or already borrowed\n",
            1,
        );
    }
}
