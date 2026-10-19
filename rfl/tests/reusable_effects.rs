//! Reusable recipes and shared resource identity, on native and Ripple targets.
use std::path::PathBuf;
struct Program(PathBuf);
impl Program {
    fn new(source: &str) -> Self {
        Self::standalone(&format!("{PRELUDE}\n{source}"))
    }
    fn standalone(source: &str) -> Self {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let dir = std::env::temp_dir().join(format!(
            "rfl-reusable-{}-{}",
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
const PRELUDE: &str = r#"
import {Result} from "std/result";
import {Effect,run,attempt,access,succeed,fail,sync,succeed_with,fail_with,map,flat_map,map_error,catch_all,provide,provide_with,from_async,from_future,from_owned,into_owned,flat_map_owned,catch_all_owned,ensuring,scoped} from "std/effect";
import {OwnedEffect,succeed as succeed_once,attempt as attempt_once,run as run_once} from "std/effect_once";
import {Environment,with_environment} from "std/effect_env";
import {Shared,shared,share,with_shared} from "std/shared";
import {Copy} from "std/value";
import {Future,Poll,Pending,Ready,yield_now} from "std/future";
import {Reactor,block_on,block_on_with_budget} from "std/executor";
import {Context,task_set} from "std/task";
struct Clock {time:u32}
impl Reactor for Clock {
 fn now(&self)->u32{self.time}
 fn turn(&mut self,wait:Option<u32>)->bool{self.time+=1;true}
}
fn execute<R,E,A>(effect:Effect<R,E,A>,value:R)->Result<A,E> {
 let mut f=run(effect,move(value));let mut r=Clock{time:0};block_on(&mut f,&mut r).unwrap()
}
fn token(n:i16)->Owned<i16>{owned(n,fn(n:&mut i16)=>{println(*n);})}
fn require_copy<T:Copy>(value:T){}
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
fn same_recipe_repeats_and_composition_remains_lazy() {
    assert_runs(
        r#"
 fn main(){
  let p:Effect<i16,String,i16> = access(fn(n:&mut i16)=>{println("run");*n+=1;*n});
  let p=flat_map(map(p,fn(n:i16)=>n*2),fn(n:i16)=>succeed<i16,String,i16>(n));
  require_copy(p);let copy=p;let unused=run(p,0);drop(move(unused));
  println("built");println(execute(p,20));println(execute(copy,40));println(execute(p,0));
 }
 "#,
        "built\nrun\nOk(42)\nrun\nOk(82)\nrun\nOk(2)\n",
        0,
    );
}
#[test]
fn fresh_owned_success_and_error_factories_repeat_without_duplication() {
    assert_runs(
        r#"
 fn main(){
  let p:Effect<unit,String,Owned<i16>> = succeed_with(fn()=>{println("new");token(7)});
  println("built");for(i in 0..2){let n=execute(p,{}).unwrap();println(*n.borrow());drop(move(n));}
  let p:Effect<unit,Owned<i16>,i16> = fail_with(fn()=>token(9));
  let p=map(p,fn(n:i16)=>{println("unexpected");n});
  for(i in 0..2){match execute(p,{}){Err(e)=>{println("error");drop(move(e));},_=>{}}}
 }
 "#,
        "built\nnew\n7\n7\nnew\n7\n7\nerror\n9\nerror\n9\n",
        0,
    );
}
#[test]
fn shared_handles_keep_owned_payload_alive_and_observe_same_identity() {
    assert_runs(
        r#"
 fn main(){
  let value=shared(token(40),fn(n:&mut Owned<i16>)=>println("last handle"));
  let p:Effect<unit,String,Shared<Owned<i16>>> = succeed(value);
  let a=execute(p,{}).unwrap();let b=execute(p,{}).unwrap();
  drop(move(value));drop(move(p));
  with_shared(a,once fn(n:&mut Owned<i16>)=>{let n=n.borrow_mut();*n+=1;});
  println(with_shared(b,once fn(n:&mut Owned<i16>)=>*n.borrow()));
  drop(move(a));println("one left");drop(move(b));println("done");
 }
 "#,
        "41\none left\nlast handle\n41\ndone\n",
        0,
    );
}
#[test]
fn shared_handles_survive_cow_containers_structs_and_reusable_closures() {
    assert_runs(
        r#"
 struct Holder {value:Shared<Owned<i16>>}
 fn main(){
  let value=share(token(5));let holder=Holder{value:value};
  let mut values:Vec<Shared<Owned<i16>>> = [holder.value];let copy=values;
  let get=fn()=>value;let other=get;require_copy(holder);require_copy(values);
  drop(move(value));drop(move(holder));values.clear();drop(move(values));drop(move(copy));
  let last=other();drop(move(get));drop(move(other));
  println(with_shared(last,once fn(n:&mut Owned<i16>)=>*n.borrow()));
  drop(move(last));println("done");
 }
 "#,
        "5\n5\ndone\n",
        0,
    );
}
#[test]
fn shared_access_retains_owner_if_callback_drops_another_alias() {
    assert_runs(
        r#"
 fn main(){let value=share(token(3));let other=value;
  with_shared(value,once fn(n:&mut Owned<i16>)=>{drop(move(other));println(*n.borrow());});
  println("alive");drop(move(value));println("done");}
 "#,
        "3\nalive\n3\ndone\n",
        0,
    );
}
#[test]
fn shared_views_expire_and_reentrant_visits_fault() {
    for body in [
        r#"let value=share(1);let env=match value{Shared(owner,view)=>view};drop(move(value));with_environment(env,once fn(n:&mut i16)=>*n);"#,
        r#"let value=share(1);let alias=value;with_shared(value,once fn(n:&mut i16)=>with_shared(alias,once fn(m:&mut i16)=>*m));"#,
    ] {
        assert_runs(
            &format!("fn main(){{{body}}}"),
            "error: effect environment is unavailable or already borrowed\n",
            1,
        );
    }
}
#[test]
fn structural_copy_proofs_include_recursive_values_and_phantom_owned_types() {
    assert_runs(
        r#"
 type List<A> = Nil | Cons(A,List<A>);
 type Phantom<A> = Phantom;
 struct Record {name:String,items:Vec<i16>}
 fn forward<A:Copy>(a:A){require_copy(a);}
 fn callback(n:Owned<i16>)->Owned<i16>{move(n)}
 fn main(){
  forward(Cons(1,Nil));let phantom:Phantom<Owned<i16>> = Phantom;forward(phantom);forward(Record{name:"yes",items:[1,2]});
  forward((true,42,"hi"));let a:[i16;2]=[1,2];forward(a);
  let p:Effect<Owned<i16>,Owned<i16>,Owned<i16>> = sync(fn()=>token(1));forward(p);
  let f:fn(Owned<i16>)->Owned<i16> = callback;forward(f);
  println("copy proofs");
 }
 "#,
        "copy proofs\n",
        0,
    );
}
#[test]
fn copy_proofs_and_exclusive_transfers_cannot_be_forged() {
    for (source, diagnostic) in [
        ("type Node = End | Next(Node,Owned<i16>);fn bad(v:Node){require_copy(move(v));}fn main(){}", "does not implement `Copy`"),
        ("fn main(){require_copy(token(1));}", "does not implement `Copy`"),
        ("struct Record{value:Owned<i16>} fn main(){require_copy(Record{value:token(1)});}","does not implement `Copy`"),
        ("fn main(){require_copy((1,token(2)));}","does not implement `Copy`"),
        ("type List<A> = Nil | Cons(A,List<A>); fn main(){require_copy(Cons(token(1),Nil));}","does not implement `Copy`"),
        ("fn main(){let a:[Owned<i16>;1]=[token(1)];require_copy(move(a));}","does not implement `Copy`"),
        ("fn main(){let p:Effect<Owned<i16>,String,i16> = access(fn(n:&mut Owned<i16>)=>*n.borrow());let supplied:Effect<unit,String,i16> = provide(p,token(1));}","does not implement `Copy`"),
        ("fn main(){require_copy([token(1)]);}", "does not implement `Copy`"),
        ("fn main(){let f:once fn()->i16=once fn()=>1;require_copy(move(f));}", "does not implement `Copy`"),
        ("async fn work()->i16{1} fn main(){require_copy(work());}", "does not implement `Copy`"),
        ("fn unbounded<A>(a:A){require_copy(move(a));} fn main(){}", "does not implement `Copy`"),
        ("fn abstract_copy<F<_>,A>(a:F<A>){require_copy(move(a));} fn main(){}", "does not implement `Copy`"),
        ("impl Copy for Owned<i16>{} fn main(){}", "cannot be implemented manually"),
        ("fn main(){let v=token(1);let s=share(move(v));v.borrow();}", "moved"),
        ("fn main(){let v=token(1);let p:Effect<unit,String,Owned<i16>> = succeed(move(v));}", "does not implement `Copy`"),
        ("fn main(){let v=token(1);let p:Effect<unit,String,i16> = attempt(fn(env:Environment<unit>)=>Ok(*v.borrow()));}", "resource"),
        ("fn main(){let p:Effect<unit,String,i16> = succeed(1);let f=run(p,{});let copy=f;}", "cannot be copied"),
        ("fn main(){let p:OwnedEffect<unit,String,i16> = succeed_once(1);let copy=p;}", "cannot be copied"),
        ("fn main(){let s=__share_owned(1);}", "requires Owned"),
        ("fn main(){let owner=token(1);let s=__share_owned(owner);}", "cannot be copied"),
        ("fn main(){let s=__share_owned(token(1));s.borrow_mut();}", "unknown function"),
        ("fn main(){let mut s=share(1);let f=fn()=>{s=share(2);};}", "cannot be captured mutably"),
        ("fn main(){let s=share(1);let r=with_shared(s,once fn(n:&mut i16)=>n);}", "reference"),
    ] {
        let program=Program::new(source);
        let error=rfl::check_file(&program.0).unwrap_err();
        assert!(error.message.contains(diagnostic), "expected {diagnostic}: {error}");
    }
}
#[test]
fn a_local_trait_named_copy_does_not_get_compiler_privileges() {
    let error =
        rfl::check_source("trait Copy{} fn need<A:Copy>(a:A){} fn main(){need(42);}").unwrap_err();
    assert!(error.message.contains("does not implement"));
    rfl::check_source(
        "trait Copy{} impl Copy for i16{} fn need<A:Copy>(a:A){} fn main(){need(42);}",
    )
    .unwrap();
    // Imported aliases still refer to the authenticated std marker.
    let p = Program::new(
        r#"import {Copy as Duplicable} from "std/value";fn need<A:Duplicable>(a:A){} fn main(){need(42);}"#,
    );
    rfl::check_file(&p.0).unwrap();
}
#[test]
fn provision_copies_values_acquires_fresh_owners_or_shares_identity_explicitly() {
    assert_runs(
        r#"
 fn main(){
  let p:Effect<i16,String,i16> = access(fn(n:&mut i16)=>{*n+=1;*n});
  let p:Effect<unit,String,i16> = provide(p,40);println(execute(p,{}));println(execute(p,{}));
  let p:Effect<Owned<i16>,String,i16> = access(fn(n:&mut Owned<i16>)=>{let n=n.borrow_mut();*n+=1;*n});
  let p:Effect<unit,String,i16> = provide_with(p,fn()=>{println("acquire");token(40)});
  let unused=run(p,{});drop(move(unused));println("built");println(execute(p,{}));println(execute(p,{}));
  let s=share(token(40));
  let p:Effect<Shared<Owned<i16>>,String,i16> = access(fn(s:&mut Shared<Owned<i16>>)=>with_shared(*s,once fn(n:&mut Owned<i16>)=>{let n=n.borrow_mut();*n+=1;*n}));
  let p:Effect<unit,String,i16> = provide(p,s);println(execute(p,{}));println(execute(p,{}));drop(s);drop(move(p));
 }
 "#,
        "Ok(41)\nOk(41)\nbuilt\nacquire\n41\nOk(41)\nacquire\n41\nOk(41)\nOk(41)\nOk(42)\n42\n",
        0,
    );
}
#[test]
fn overlapping_runs_keep_separate_environments_and_cancel_independently() {
    assert_runs(
        r#"
 async fn later(env:Environment<Owned<i16>>)->Result<i16,String>{
  let n=with_environment(env,once fn(n:&mut Owned<i16>)=>*n.borrow());yield_now().await;Ok(n)
 }
 fn main()->i16{
  let p:Effect<Owned<i16>,String,i16> = from_async(later);
  let p=ensuring(p,fn()=>println("finally"));
  let mut a=run(p,token(1));let mut b=run(p,token(2));
  let q=task_set(1,0).unwrap();let w=q.allocate().unwrap();let mut cx=Context{waker:w,now:0};
  match a.poll(&mut cx){Pending=>{},_=>{return 1;}}
  match b.poll(&mut cx){Pending=>{},_=>{return 2;}}
  a.cancel();a.cancel();println("a cancelled");
  match b.poll(&mut cx){Ready(Ok(2))=>{},_=>{return 3;}}println("b done");
  println(execute(p,token(3)));0
 }
 "#,
        "finally\n1\na cancelled\nfinally\n2\nb done\nfinally\n3\nOk(3)\n",
        0,
    );
}
#[test]
fn simultaneous_shared_runs_keep_payload_until_both_are_gone() {
    assert_runs(
        r#"
 async fn later(value:Shared<Owned<i16>>)->i16{
  with_shared(value,once fn(n:&mut Owned<i16>)=>{let n=n.borrow_mut();*n+=1;});
  yield_now().await;with_shared(value,once fn(n:&mut Owned<i16>)=>*n.borrow())
 }
 fn main()->i16{
  let value=share(token(40));
  let p:Effect<unit,String,i16> = from_future(fn(env:Environment<unit>)=>later(value));
  let mut a=run(p,{});let mut b=run(p,{});drop(move(value));drop(move(p));
  let q=task_set(1,0).unwrap();let w=q.allocate().unwrap();let mut cx=Context{waker:w,now:0};
  match a.poll(&mut cx){Pending=>{},_=>{return 1;}}
  match b.poll(&mut cx){Pending=>{},_=>{return 2;}}
  a.cancel();println("a cancelled");
  match b.poll(&mut cx){Ready(Ok(n))=>println(n),_=>{return 3;}}
  drop(move(a));drop(move(b));println("done");0
 }
 "#,
        "a cancelled\n42\n42\ndone\n",
        0,
    );
}
#[test]
fn consuming_subprogram_adapters_acquire_per_run_and_preserve_environment() {
    assert_runs(
        r#"
 fn use_owned(value:Owned<i16>)->OwnedEffect<i16,String,i16>{
  attempt_once(once fn(env:Environment<i16>)=>Ok(with_environment(env,once fn(n:&mut i16)=>*n)+*value.borrow()))
 }
 fn main(){
  let fresh:Effect<i16,String,Owned<i16>> = sync(fn()=>token(2));
  let p=flat_map_owned(fresh,use_owned);println(execute(p,40));println(execute(p,20));
  let p:Effect<i16,String,i16> = from_owned(fn()=>use_owned(token(3)));
  println(execute(p,40));println(execute(p,20));
  let once=into_owned(p);let mut f=run_once(move(once),10);let mut clock=Clock{time:0};println(block_on(&mut f,&mut clock).unwrap());
 }
 "#,
        "2\nOk(42)\n2\nOk(22)\n3\nOk(43)\n3\nOk(23)\n3\nOk(13)\n",
        0,
    );
}
#[test]
fn typed_failures_and_recovery_repeat_without_stale_state() {
    assert_runs(
        r#"
 fn main(){
  let p:Effect<unit,String,i16> = fail("bad");let p=map(p,fn(n:i16)=>{println("unexpected");n});
  let p=map_error(p,fn(e:String)=>{println(e);42});
  let p:Effect<unit,String,i16> = catch_all(p,fn(n:i16)=>succeed(n));
  println(execute(p,{}));println(execute(p,{}));
 }
 "#,
        "bad\nOk(42)\nbad\nOk(42)\n",
        0,
    );
}
#[test]
fn finalizers_and_scoped_resources_register_fresh_on_every_run() {
    assert_runs(r#"
 fn main(){
  let acquire:Effect<unit,String,Owned<i16>> = sync(fn()=>{println("acquire");token(7)});
  let p=scoped(acquire,fn(env:Environment<Owned<i16>>)=>attempt<unit,String,i16>(fn(root:Environment<unit>)=>Ok(with_environment(env,once fn(n:&mut Owned<i16>)=>*n.borrow()))),fn(n:&mut Owned<i16>)=>println("release"));
  let p=ensuring(ensuring(p,fn()=>println("inner")),fn()=>println("outer"));
  let unused=run(p,{});drop(move(unused));println("built");println(execute(p,{}));println(execute(p,{}));
 }
 "#, "built\nacquire\nrelease\n7\ninner\nouter\nOk(7)\nacquire\nrelease\n7\ninner\nouter\nOk(7)\n", 0);
}
#[test]
fn generic_fp_mapping_binding_and_factory_injection_accept_reusable_effects() {
    assert_runs(
        r#"
 import {map as fmap,bind,pure_with} from "std/fp";
 fn main(){
  let p:Effect<unit,String,Owned<i16>> = pure_with<Effect<unit,String,_>,_>(fn()=>token(40));
  let p=fmap<Effect<unit,String,_>,_,_>(p,fn(n:Owned<i16>)=>*n.borrow()+1);
  let p=bind<Effect<unit,String,_>,_,_>(p,fn(n:i16)=>succeed<unit,String,i16>(n+1));
  println(execute(p,{}));println(execute(p,{}));
  println(bind<Option,_,_>(pure_with<Option,_>(fn()=>1),fn(n:i16)=>Some(n+1)));
  println(bind<Vec,_,_>([1,2],fn(n:i16)=>[n,n]));
  let r:Result<i16,String> = pure_with<Result<_,String>,_>(fn()=>1);
  println(bind<Result<_,String>,i16,i16>(r,fn(n:i16)=>Ok(n+1)));
 }
 "#,
        "40\nOk(42)\n40\nOk(42)\nSome(2)\n[1, 1, 2, 2]\nOk(2)\n",
        0,
    );
}

#[test]
fn hidden_argument_snapshots_and_lexical_aliases_do_not_delay_last_release() {
    assert_runs(
        r#"
 fn visit(s:SharedOwner){}
 fn main(){
  let owner=owned(1,fn(n:&mut i16)=>println("closed"));
  let s=__share_owned(move(owner));visit(s);{let alias=s;}
  drop(move(s));println("after close");
 }
 "#,
        "closed\nafter close\n",
        0,
    );
}

#[test]
fn scoped_failure_and_cancellation_release_each_runs_own_resources() {
    assert_runs(r#"
 struct Never {value:Owned<i16>}
 impl Future for Never {
  type Output=i16;
  fn poll(&mut self,cx:&mut Context)->Poll<i16>{cx.waker.wake();Pending}
  fn cancel(&mut self){println("cancel child");}
 }
 fn main()->i16{
  let acquire:Effect<unit,String,Owned<i16>> = sync(fn()=>token(1));
  let failed=scoped(acquire,fn(view:Environment<Owned<i16>>)=>fail<unit,String,i16>("bad"),fn(n:&mut Owned<i16>)=>println("release"));
  println(execute(failed,{}));println(execute(failed,{}));
  let p=scoped(acquire,fn(view:Environment<Owned<i16>>)=>from_future<unit,String,Never>(fn(env:Environment<unit>)=>Never{value:token(2)}),fn(n:&mut Owned<i16>)=>println("release"));
  let p=ensuring(p,fn()=>println("finally"));
  let mut clock=Clock{time:0};
  for(i in 0..2){let mut f=run(p,{});match block_on_with_budget(&mut f,&mut clock,1){Err(_)=>{},_=>{return 1;}}}
  0
 }
 "#, "release\n1\nErr(bad)\nrelease\n1\nErr(bad)\ncancel child\n2\nrelease\n1\nfinally\ncancel child\n2\nrelease\n1\nfinally\n", 0);
}
#[test]
fn runs_can_escape_views_but_they_do_not_retain_a_finished_environment() {
    assert_runs(
        r#"
 fn main(){
  let p:Effect<Owned<i16>,String,Environment<Owned<i16>>> = attempt(fn(env:Environment<Owned<i16>>)=>Ok(env));
  let first=execute(p,token(1)).unwrap();let second=execute(p,token(2)).unwrap();
  with_environment(first,once fn(n:&mut Owned<i16>)=>println("unexpected"));
 }
 "#,
        "1\n2\nerror: effect environment is unavailable or already borrowed\n",
        1,
    );
}
#[test]
fn task_scopes_can_spawn_the_same_recipe_and_keep_results_independent() {
    assert_runs(
        r#"
 import {task_scope} from "std/scope";
 async fn later(env:Environment<i16>)->Result<i16,String>{yield_now().await;Ok(with_environment(env,once fn(n:&mut i16)=>*n))}
 fn main()->i16{
  let p:Effect<i16,String,i16> = from_async(later);
  let mut scope=task_scope(2,0).unwrap();
  let mut a=scope.spawn(run(p,1)).unwrap();let mut b=scope.spawn(run(p,2)).unwrap();
  let mut clock=Clock{time:0};match scope.run_with_budget(&mut clock,8){Ok(_)=>{},_=>{return 1;}}
  println(block_on(&mut a,&mut clock).unwrap().unwrap());println(block_on(&mut b,&mut clock).unwrap().unwrap());0
 }
 "#,
        "Ok(1)\nOk(2)\n",
        0,
    );
}
#[test]
fn reusable_recipe_chains_run_twice_and_cancel_without_consuming_the_recipe() {
    let template = include_str!("fixtures/reusable_chain.rfl");
    let p = Program::standalone(
        &template.replace("const DEPTH: u16 = 64;", "const DEPTH: u16 = 6000;"),
    );
    let run = rfl::run_native_file(&p.0, false).unwrap_or_else(|e| panic!("{e}"));
    assert_eq!(run.status, 0, "{}", run.stdout);
    assert_eq!(run.stdout, "repeat/cancel\n");

    // Count guest instructions, not host wall time. Old heap scans exceed both
    // budgets; doubling a persistent graph must stay close to linear growth.
    let small = measured_chain(template, 128);
    let large = measured_chain(template, 256);
    assert!(
        large * 10 < small * 24,
        "superlinear chain cost: {small} -> {large}"
    );
}

fn measured_chain(template: &str, depth: u64) -> u64 {
    use std::process::Command;
    fn tool(cmd: &mut Command) -> std::process::Output {
        let output = cmd.output().unwrap();
        assert!(
            output.status.success(),
            "{cmd:?}: {}\n{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
        output
    }
    let program = Program::standalone(&template.replace(
        "const DEPTH: u16 = 64;",
        &format!("const DEPTH: u16 = {depth};"),
    ));
    let asm = program.0.with_extension("asm");
    let object = program.0.with_extension("pobj");
    let binary = program.0.with_extension("bin");
    let report = program.0.with_extension("json");
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    std::fs::write(&asm, rfl::compile_file_to_asm(&program.0).unwrap()).unwrap();
    tool(
        Command::new(root.join("src/ripple-asm/target/release/rasm"))
            .arg("assemble")
            .arg(&asm)
            .arg("-o")
            .arg(&object)
            .args(["--bank-size", "64000", "--max-immediate", "65535"]),
    );
    tool(
        Command::new(root.join("src/ripple-asm/target/release/rlink"))
            .arg(root.join("runtime/crt0.pobj"))
            .arg(&object)
            .arg(root.join("runtime/libruntime.par"))
            .args(["-f", "binary", "--bank-size", "64000", "-o"])
            .arg(&binary),
    );
    let budget = depth * 600_000 + 2_000_000;
    let output = tool(
        Command::new(root.join("target/release/rvm"))
            .arg(&binary)
            .args([
                "--headless",
                "--exit-code",
                "--memory",
                "16384000",
                "--timeout",
                "20",
                "--report",
            ])
            .arg(&report)
            .arg("--max-instructions")
            .arg(budget.to_string()),
    );
    assert_eq!(String::from_utf8(output.stdout).unwrap(), "repeat/cancel\n");
    let report: serde_json::Value =
        serde_json::from_slice(&std::fs::read(report).unwrap()).unwrap();
    assert_eq!(report["success"], true);
    assert_eq!(report["stop_reason"], "halted");
    report["instructions"].as_u64().unwrap()
}

#[test]
fn borrowed_temporary_receivers_keep_their_shared_payload_alive() {
    assert_runs(
        r#"
 struct Holder {value:Shared<Owned<i16>>}
 impl Holder {fn read(&self)->i16{with_shared(self.value,once fn(n:&mut Owned<i16>)=>*n.borrow())}}
 fn main(){println(Holder{value:share(token(42))}.read());println("after read");}
 "#,
        "42\nafter read\n42\n",
        0,
    );
}

#[test]
fn shared_owner_copies_and_lexical_cleanup_run_on_the_brainfuck_cpu() {
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
 fn visit(value:SharedOwner){putc(65);}
 fn main(){
  let original=owned(0,fn(n:&mut i16)=>{putc(66);});
  let s=__share_owned(move(original));visit(s);{let alias=s;}
  drop(move(s));putc(67);
 }
 "#;
    let file = Program::new("");
    let dir = file.0.parent().unwrap();
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
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
    let output = command(
        Command::new(root.join("target/release/rbf"))
            .arg(&bf)
            .args(["--jit", "--headless", "--timeout", "30", "--exit-code"]),
    );
    assert_eq!(String::from_utf8(output.stdout).unwrap(), "ABC");
}

#[test]
fn unpolled_shared_runs_release_captures_without_running_callbacks() {
    assert_runs(
        r#"
 fn main(){
  let value=share(token(7));
  let p:Effect<unit,String,Shared<Owned<i16>>> = attempt(fn(env:Environment<unit>)=>{println("unexpected");Ok(value)});
  let p=ensuring(move(p),fn()=>println("unexpected finalizer"));
  let f=run(p,{});drop(move(value));drop(move(p));println("held by unpolled run");
  drop(move(f));println("done");
 }
 "#,
        "held by unpolled run\n7\ndone\n",
        0,
    );
}
#[test]
fn captured_values_use_cow_and_owning_slices_retain_shared_elements() {
    assert_runs(
        r#"
 fn main(){
  let p:Effect<unit,String,Vec<i16>> = succeed([1]);let mut a=execute(p,{}).unwrap();a.push(2);
  println(a);println(execute(p,{}));
  let values=[share(token(9))];let slice=values[0..1];drop(move(values));
  println(with_shared(slice[0],once fn(n:&mut Owned<i16>)=>*n.borrow()));
  drop(move(slice));println("slice dropped");
 }
 "#,
        "[1, 2]\nOk([1])\n9\n9\nslice dropped\n",
        0,
    );
}

#[test]
fn owned_error_recovery_constructs_a_fresh_consuming_child_each_run() {
    assert_runs(
        r#"
 fn recover(error:Owned<i16>)->OwnedEffect<unit,String,i16>{
  attempt_once(once fn(env:Environment<unit>)=>Ok(*error.borrow()+1))
 }
 fn main(){
  let p:Effect<unit,Owned<i16>,i16> = fail_with(fn()=>token(9));
  let p=catch_all_owned(p,recover);println(execute(p,{}));println(execute(p,{}));
 }
 "#,
        "9\nOk(10)\n9\nOk(10)\n",
        0,
    );
}

#[test]
fn structural_copy_proof_visits_shared_type_dags_once() {
    use std::process::{Command, Stdio};
    use std::time::{Duration, Instant};
    let mut ty = "i16".to_string();
    for _ in 0..64 {
        ty = format!("Pair<{ty}>");
    }
    let p = Program::new(&format!(
        "type Pair<T> = Pair(T,T);fn proof(value:{ty}){{require_copy(value);}} fn main(){{}}"
    ));
    let mut child = Command::new(env!("CARGO_BIN_EXE_rfl"))
        .arg("check")
        .arg(&p.0)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        if let Some(status) = child.try_wait().unwrap() {
            let output = child.wait_with_output().unwrap();
            assert!(
                status.success(),
                "{}",
                String::from_utf8_lossy(&output.stderr)
            );
            break;
        }
        if Instant::now() >= deadline {
            child.kill().unwrap();
            let _ = child.wait();
            panic!("structural Copy proof must visit a shared type graph once, not expand its exponentially many paths");
        }
        std::thread::sleep(Duration::from_millis(10));
    }
}
