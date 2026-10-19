#[path = "support/fp.rs"]
mod support;
use support::{parity, Fixture};
const PRELUDE: &str = r#"
import {Result} from "std/result";
import {Option} from "std/option";
import {Apply,ApplyOnce,map2,zip,map2_once,sequence,cartesian_map2} from "std/applicative";
import {traverse,traverse_with,traverse_once} from "std/traverse";
import {Effect,run,succeed,sync,attempt,fail,from_async,from_future,ensuring,traverse_owned_with} from "std/effect";
import {OwnedEffect,run as run_once,succeed as once_value,from_async as once_async,ensuring as once_ensuring} from "std/effect_once";
import {Environment} from "std/effect_env";
import {Future,Poll,Pending,Ready,yield_now} from "std/future";
import {Context,task_set} from "std/task";
import {block_on,block_on_with_budget,Reactor} from "std/executor";
struct Clock{time:u32}
impl Reactor for Clock{fn now(&self)->u32{self.time}fn turn(&mut self,w:Option<u32>)->bool{self.time+=1;true}}
fn execute<R,E,A>(p:Effect<R,E,A>,env:R)->Result<A,E>{let mut f=run(p,move(env));let mut clock=Clock{time:0};block_on(&mut f,&mut clock).unwrap()}
fn token(n:i16)->Owned<i16>{owned(n,fn(n:&mut i16)=>println(*n))}
fn sum(a:Owned<i16>,b:Owned<i16>)->i16{*a.borrow()+*b.borrow()}
"#;
fn check(source: &str, expected: &str) {
    parity(&format!("{PRELUDE}\n{source}"), expected)
}

#[test]
fn eager_apply_moves_owners_and_selects_the_left_failure() {
    check(
        r#"fn main(){
 println(map2(Some(token(20)),Some(token(22)),sum));
 let a:Result<i16,String> =Err("left");let b:Result<i16,String> =Err("right");
 println(map2<Result<_,String>,_,_,_>(a,b,fn(a:i16,b:i16)=>a+b));
 let a:Option<Owned<i16>> =None;println(map2(move(a),Some(token(9)),sum));
 let r:Result<Owned<i16>,String> =Ok(token(7));let e:Result<Owned<i16>,String> =Err("bad");
 println(map2<Result<_,String>,_,_,_>(move(r),move(e),sum));
 }"#,
        "22\n20\nSome(42)\nErr(left)\n9\nNone\n7\nErr(bad)\n",
    );
}

#[test]
fn consuming_apply_releases_used_and_skipped_callback_captures() {
    check(
        r#"fn main(){
 let capture=token(40);println(map2_once(Some(1),Some(1),once fn(a:i16,b:i16)=>*capture.borrow()+a+b));
 let capture=token(9);let a:Option<i16> =None;
 println(map2_once(a,Some(1),once fn(a:i16,b:i16)=>*capture.borrow()+a+b));
 }"#,
        "40\nSome(42)\n9\nNone\n",
    );
}

#[test]
fn applicative_function_values_support_reusable_and_fresh_consuming_functions() {
    check(
        r#"import {ap,ap_owned} from "std/applicative";
 fn main(){
 let plus:fn(i16)->i16=fn(n:i16)=>n+1;println(ap(Some(plus),Some(41)));
 let n=token(40);let f:once fn(i16)->i16=once fn(a:i16)=>*n.borrow()+a;
 println(ap_owned(Some(move(f)),Some(2)));
 let make:Effect<unit,String,once fn(i16)->i16> =sync(fn()=>{let n=token(40);once fn(a:i16)=>*n.borrow()+a});
 let p=ap_owned<Effect<unit,String,_>,_,_>(make,succeed<unit,String,i16>(2));
 println(execute(p,{}));println(execute(p,{}));
 }"#,
        "Some(42)\n40\nSome(42)\n40\nOk(42)\n40\nOk(42)\n",
    );
}

#[test]
fn sequence_eager_empty_order_and_owned_values() {
    check(
        r#"fn main(){
 println(sequence<Option,i16>([]));println(sequence<Option,i16>([Some(1),Some(2)]));
 println(sequence<Result<_,String>,i16>([Ok(1),Err("first"),Err("last")]));
 let values=sequence<Option,Owned<i16>>([Some(token(1)),Some(token(2))]).unwrap();
 let mut values=move(values);while(!values.is_empty()){let n=values.pop().unwrap();println(*n.borrow());}
 }"#,
        "Some([])\nSome([1, 2])\nErr(first)\n2\n2\n1\n1\n",
    );
}

#[test]
fn eager_traversal_short_circuits_and_transfers_exclusive_inputs() {
    check(
        r#"fn main(){
 let mut seen:i16=0;
 println(traverse<Option,i16,i16>([1,2,3],fn(n:i16)=>{seen=seen*10+n;if(n==2){None}else{Some(n)}}));println(seen);
 let values=[token(3),token(4)];
 println(traverse_once<Result<_,String>,Owned<i16>,i16>(move(values),fn(n:Owned<i16>)=>Ok(*n.borrow()+1)));
 }"#,
        "None\n12\n3\n4\nOk([4, 5])\n",
    );
}

#[test]
fn cartesian_vectors_preserve_pair_order_and_create_fresh_owned_outputs() {
    check(
        r#"fn main(){
 println(cartesian_map2([1,2],[3,4],fn(a:i16,b:i16)=>a*10+b));
 let mut out=cartesian_map2([1,2],[3],fn(a:i16,b:i16)=>token(a*10+b));
 while(!out.is_empty()){let n=out.pop().unwrap();println(*n.borrow());}
 println(cartesian_map2<i16,i16,i16>([],[1],fn(a:i16,b:i16)=>a+b));
 }"#,
        "[13, 14, 23, 24]\n23\n23\n13\n13\n[]\n",
    );
}

#[test]
fn sequential_apply_obeys_identity_composition_and_factory_homomorphism() {
    check(
        r#"import {pure_with} from "std/fp";
 fn main(){
 let identity=map2(Some(42),Some({}),fn(a:i16,u:unit)=>a);println(identity.unwrap()==42);
 let a=map2(map2(Some(1),Some(2),fn(a:i16,b:i16)=>a+b),Some(3),fn(a:i16,b:i16)=>a+b);
 let b=map2(Some(1),map2(Some(2),Some(3),fn(a:i16,b:i16)=>a+b),fn(a:i16,b:i16)=>a+b);println(a.unwrap()==b.unwrap());
 let combined=map2(pure_with<Option,_>(fn()=>20),pure_with<Option,_>(fn()=>22),fn(a:i16,b:i16)=>a+b);let expected=pure_with<Option,_>(fn()=>42);println(combined.unwrap()==expected.unwrap());
 let left:Effect<unit,String,i16> =succeed(20);let right:Effect<unit,String,i16> =succeed(22);
 println(execute(map2<Effect<unit,String,_>,_,_,_>(left,right,fn(a:i16,b:i16)=>a+b),{}));
 }"#,
        "true\ntrue\ntrue\nOk(42)\n",
    );
}

#[test]
fn effect_map2_is_lazy_reusable_and_accepts_move_only_successes() {
    check(
        r#"fn main(){
 let left:Effect<unit,String,Owned<i16>> =sync(fn()=>{println("left");token(20)});
 let right:Effect<unit,String,Owned<i16>> =sync(fn()=>{println("right");token(22)});
 let p=map2<Effect<unit,String,_>,_,_,_>(left,right,sum);let f=run(p,{});drop(move(f));println("built");
 println(execute(p,{}));println(execute(p,{}));
 }"#,
        "built\nleft\nright\n22\n20\nOk(42)\nleft\nright\n22\n20\nOk(42)\n",
    );
}

#[test]
fn effect_apply_failures_skip_work_and_release_retained_left_values() {
    check(
        r#"fn main(){
 let left:Effect<unit,String,Owned<i16>> =fail("left");
 let right:Effect<unit,String,Owned<i16>> =sync(fn()=>{println("unexpected");token(2)});
 println(execute(map2<Effect<unit,String,_>,_,_,_>(left,right,sum),{}));
 let left:Effect<unit,String,Owned<i16>> =sync(fn()=>token(1));
 let right:Effect<unit,String,Owned<i16>> =fail("right");
 println(execute(map2<Effect<unit,String,_>,_,_,_>(left,right,sum),{}));
 }"#,
        "Err(left)\n1\nErr(right)\n",
    );
}

#[test]
fn sequential_apply_transfers_owned_errors_without_copying_them() {
    check(
        r#"import {fail_with} from "std/effect";
 fn main(){
 let left:Effect<unit,Owned<i16>,i16> = fail_with(fn()=>token(9));
 let right:Effect<unit,Owned<i16>,i16> = sync(fn()=>{println("unexpected");1});
 let p=map2<Effect<unit,Owned<i16>,_>,_,_,_>(left,right,fn(a:i16,b:i16)=>a+b);
 for(i in 0..2){match execute(p,{}){Err(e)=>{println(*e.borrow());drop(move(e));},Ok(_)=>{}}}
 }"#,
        "9\n9\n9\n9\n",
    );
}

#[test]
fn cancellation_drops_right_child_before_the_retained_left_result() {
    check(
        r#"struct Never{value:Owned<i16>}
 impl Future for Never{type Output=Owned<i16>;fn poll(&mut self,cx:&mut Context)->Poll<Owned<i16>>{cx.waker.wake();Pending}fn cancel(&mut self){println("cancel right");}}
 fn main()->i16{
 let left:Effect<unit,String,Owned<i16>> =sync(fn()=>token(1));
 let right:Effect<unit,String,Owned<i16>> =from_future(fn(env:Environment<unit>)=>Never{value:token(2)});
 let p=ensuring(map2<Effect<unit,String,_>,_,_,_>(left,right,sum),fn()=>println("finally"));
 let mut f=run(p,{});let q=task_set(1,0).unwrap();let w=q.allocate().unwrap();let mut cx=Context{waker:w,now:0};
 match f.poll(&mut cx){Pending=>{},_=>{return 1;}}f.cancel();f.cancel();0
 }"#,
        "cancel right\n2\n1\nfinally\n",
    );
}

#[test]
fn consuming_effect_apply_owns_the_combiner_and_unused_right_program() {
    check(
        r#"import {map2 as once_pair,fail as once_fail} from "std/effect_once";
 fn main(){
 let owner=token(40);let a:OwnedEffect<unit,String,i16> =once_value(1);let b:OwnedEffect<unit,String,i16> =once_value(1);
 let p=once_pair(move(a),move(b),once fn(a:i16,b:i16)=>*owner.borrow()+a+b);
 let mut f=run_once(move(p),{});let mut c=Clock{time:0};println(block_on(&mut f,&mut c).unwrap());
 let a:OwnedEffect<unit,String,Owned<i16>> =once_fail("bad");let b:OwnedEffect<unit,String,Owned<i16>> =once_value(token(9));
 let mut f=run_once(once_pair(move(a),move(b),once fn(a:Owned<i16>,b:Owned<i16>)=>sum(move(a),move(b))),{});
 println(block_on(&mut f,&mut c).unwrap());
 }"#,
        "40\nOk(42)\n9\nErr(bad)\n",
    );
}

#[test]
fn reusable_traversal_defers_factories_and_visits_only_successful_prefixes() {
    check(
        r#"fn main(){
 let p:Effect<unit,String,Vec<i16>> =traverse_with<Effect<unit,String,_>,_,_>(fn()=>{println("inputs");[1,2,3]},fn(n:i16)=>{println(n);if(n==2){fail("stop")}else{succeed(n)}});
 let f=run(p,{});drop(move(f));println("built");println(execute(p,{}));println(execute(p,{}));
 let empty:Effect<unit,String,Vec<i16>> =traverse<Effect<unit,String,_>,i16,i16>([],fn(n:i16)=>{println("unexpected");succeed(n)});println(execute(empty,{}));
 }"#,
        "built\ninputs\n1\n2\nErr(stop)\ninputs\n1\n2\nErr(stop)\nOk([])\n",
    );
}

#[test]
fn fresh_owned_traversal_inputs_can_live_in_consuming_children_across_await() {
    check(
        r#"async fn step(n:Owned<i16>)->Result<Owned<i16>,String>{yield_now().await;Ok(move(n))}
 fn main(){
 let p:Effect<unit,String,Vec<Owned<i16>>> =traverse_owned_with(fn()=>[token(1),token(2)],fn(n:Owned<i16>)=>once_async(once fn(env:Environment<unit>)=>step(move(n))));
 for(i in 0..2){let mut out=execute(p,{}).unwrap();while(!out.is_empty()){let n=out.pop().unwrap();println(*n.borrow());}}
 }"#,
        "2\n2\n1\n1\n2\n2\n1\n1\n",
    );
}

#[test]
fn traversal_failure_releases_partial_outputs_and_unvisited_inputs() {
    check(
        r#"fn main(){
 let p:Effect<unit,String,Vec<Owned<i16>>> =traverse_owned_with(fn()=>[token(1),token(2),token(3)],fn(n:Owned<i16>)=>{
  if({let value=n.borrow();*value}==2){println("failure");once_async(once fn(env:Environment<unit>)=>reject(move(n)))}else{once_value(move(n))}
 });println(match execute(p,{}){Err(e)=>e,Ok(out)=>{drop(move(out));"unexpected"}});
 }
 async fn reject(n:Owned<i16>)->Result<Owned<i16>,String>{Err("stop")}
 "#,
        "failure\n2\n1\n3\nstop\n",
    );
}

#[test]
fn traversal_cancellation_reclaims_child_accumulator_and_remaining_inputs() {
    check(
        r#"async fn step(n:Owned<i16>)->Result<Owned<i16>,String>{if({let value=n.borrow();*value}==2){yield_now().await;}Ok(move(n))}
 fn main()->i16{
 let p:Effect<unit,String,Vec<Owned<i16>>> =traverse_owned_with(fn()=>[token(1),token(2),token(3)],fn(n:Owned<i16>)=>once_async(once fn(env:Environment<unit>)=>step(move(n))));
 let mut f=run(p,{});let q=task_set(1,0).unwrap();let w=q.allocate().unwrap();let mut cx=Context{waker:w,now:0};
 match f.poll(&mut cx){Pending=>{},_=>{return 1;}}f.cancel();f.cancel();0
 }"#,
        "2\n1\n3\n",
    );
}

#[test]
fn long_traversals_yield_and_cancel_during_reversal_or_accumulation() {
    check(
        r#"fn main()->i16{
 let mut drops:u16=0;let mut visits:u16=0;
 let p:Effect<unit,String,Vec<Owned<i16>>> = traverse_owned_with(fn()=>{
  let mut values:Vec<Owned<i16>> = [];for(i in 0..129){values.push(owned(i,fn(n:&mut i16)=>{drops+=1;}));}move(values)
 },fn(n:Owned<i16>)=>{visits+=1;once_value(move(n))});
 let q=task_set(1,0).unwrap();let w=q.allocate().unwrap();let mut cx=Context{waker:w,now:0};
 let mut first=run(p,{});match first.poll(&mut cx){Pending=>{},_=>{return 1;}}
 if(visits!=0){return 2;}first.cancel();if(drops!=129){return 3;}
 let mut second=run(p,{});let mut turns:u16=0;
 while(visits==0&&turns<8){match second.poll(&mut cx){Pending=>{},_=>{return 4;}}turns+=1;}
 if(visits==0||visits>=129){return 5;}second.cancel();second.cancel();
 if(drops!=258){return 6;}println("both stages cancelled");0
 }"#,
        "both stages cancelled\n",
    );
}

#[test]
fn generic_and_specialized_sequence_repeat_and_consume_owned_programs() {
    check(
        r#"import {sequence as effect_sequence} from "std/effect";
 import {sequence as consuming_sequence} from "std/effect_once";
 fn main(){
 let a:Effect<unit,String,i16> =succeed(1);let b:Effect<unit,String,i16> =succeed(2);
 let p=sequence<Effect<unit,String,_>,_>([a,b]);println(execute(p,{}));println(execute(p,{}));
 let p=effect_sequence([a,b]);println(execute(p,{}));
 let a:OwnedEffect<unit,String,Owned<i16>> =once_value(token(1));
 let p=sequence<OwnedEffect<unit,String,_>,_>([move(a)]);let mut f=run_once(move(p),{});let mut clock=Clock{time:0};drop(block_on(&mut f,&mut clock).unwrap());
 let a:OwnedEffect<unit,String,Owned<i16>> =once_value(token(2));
 let p=consuming_sequence([move(a)]);let mut f=run_once(move(p),{});drop(block_on(&mut f,&mut clock).unwrap());
 }"#,
        "Ok([1, 2])\nOk([1, 2])\nOk([1, 2])\n1\n2\n",
    );
}

#[test]
fn starting_a_consuming_traversal_does_not_invoke_its_factory_before_poll() {
    check(
        r#"import {traverse_with as once_traverse,start_in} from "std/effect_once";
 import {own_environment} from "std/effect_env";
 fn main()->i16{
 let mut calls:u16=0;
 let p:OwnedEffect<unit,String,Vec<i16>> = once_traverse(fn()=>{calls+=1;[1,2]},fn(n:i16)=>once_value<unit,String,i16>(n));
 let (owner,env)=own_environment({});let f=start_in(move(p),env);drop(move(f));
 if(calls!=0){return 1;}println("factory stayed lazy");0
 }"#,
        "factory stayed lazy\n",
    );
}

#[test]
fn invalid_cartesian_and_reusable_ownership_requests_are_rejected() {
    for(source,diagnostic) in [
      ("fn main(){cartesian_map2([token(1)],[1],fn(a:Owned<i16>,b:i16)=>b);}","does not implement `Copy`"),
      ("fn main(){map2([1],[2],fn(a:i16,b:i16)=>a+b);}","does not implement"),
      ("fn main(){let p:Effect<unit,String,Vec<i16>> =traverse<Effect<unit,String,_>,_,_>([token(1)],fn(n:Owned<i16>)=>succeed<unit,String,i16>(*n.borrow()));}","does not implement `Copy`"),
      ("fn main(){let n=token(1);let a:Effect<unit,String,i16> =succeed(1);map2<Effect<unit,String,_>,_,_,_>(a,a,once fn(a:i16,b:i16)=>*n.borrow());}","once fn"),
    ]{
      let f=Fixture::new(&format!("{PRELUDE}\n{source}"));let e=rfl::check_file(&f.0).unwrap_err();assert!(e.message.contains(diagnostic),"{e}");
    }
}

#[test]
fn applicative_owned_combination_executes_on_the_brainfuck_cpu() {
    support::brainfuck(
        r#"import {map2_once} from "std/applicative";
 fn main(){let n=owned(40,fn(n:&mut i16)=>{});match map2_once(Some(1),Some(1),once fn(a:i16,b:i16)=>*n.borrow()+a+b){Some(42)=>putc(79),_=>{}};putc(75);}"#,
        "OK",
    );
}

#[test]
fn traversal_instruction_cost_scales_with_items_and_owned_results() {
    let template = include_str!("fixtures/traversal.rfl");
    let small = support::measured(
        &template.replace("const DEPTH: u16 = 64;", "const DEPTH: u16 = 128;"),
        "traversed twice\n",
        55_000_000,
    );
    let large = support::measured(
        &template.replace("const DEPTH: u16 = 64;", "const DEPTH: u16 = 256;"),
        "traversed twice\n",
        110_000_000,
    );
    assert!(
        large * 10 < small * 25,
        "superlinear traversal: {small} -> {large}"
    );
}
