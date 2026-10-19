#[path = "support/fp.rs"]
mod support;

#[test]
fn option_and_vector_comprehensions_preserve_order_and_let_scope() {
    support::parity(
        r#"fn main() {
 let answer = for { a <- Some(20); b <- Some(22); yield a+b; };
 println(answer);
 println(for { a <- [1,2]; let n=a*10; b <- [3,4]; yield n+b; });
 let yield=7;let once=1;let mut sum=0;for(n in [yield,once]){sum+=n;}println(sum);
 }"#,
        "Some(42)\n[13, 14, 23, 24]\n8\n",
    );
}

fn reject(source: &str, message: &str) -> rfl::CompileError {
    let fixture = support::Fixture::new(source);
    let error = rfl::check_file(&fixture.0).unwrap_err();
    assert!(error.detail.contains(message), "{error}");
    error
}

#[test]
fn first_source_locals_and_dependent_sources_evaluate_in_order_and_short_circuit() {
    support::parity(
        r#"
 fn mark(n:i16,value:Option<i16>)->Option<i16>{println(n);value}
 fn stop()->Result<i16,String>{Err("stop")}
 fn main(){
 println(for {a<-mark(1,Some(10));let n={println(2);a+1};b<-mark(3,Some(n));yield {println(4);a+b}});
 let empty:Option<i16> = None;
 println(for {a<-mark(5,empty);b<-mark(6,Some(a));yield b});
 let result:Result<i16,String> = for<Result<_,String>> {a<-stop();let n={println(99);a};b<-Ok(n);yield b};println(result);
 }"#,
        "1\n2\n3\n4\nSome(21)\n5\nNone\nErr(stop)\n",
    );
}

#[test]
fn annotations_patterns_shadowing_nested_sugar_and_contextual_identifiers() {
    support::parity(
        r#"
 struct Pair{left:i16,right:i16}
 fn main(){
 let a=10;
 println(for {(a,b)<-Some((1,2));Pair{left,right:r}<-Some(Pair{left:a,right:b});yield left+r});
 println(a);
 println(for {yield<-Some(2);let once=3;n<-for{x<-Some(yield);yield x+once};yield n});
 println(for {n:u32<-Some(70000);yield n+1});
 println(for {n<-Some(1);let n=n+1;yield n});
 let empty:Option<i16> = for {n:i16<-None;yield n};println(empty);
 let ok:Result<i16,String> = for<Result<_,String>> {n<-Ok(42);yield n};println(ok);
 }"#,
        "Some(3)\n10\nSome(5)\nSome(70001)\nSome(2)\nNone\nOk(42)\n",
    );
}

#[test]
fn helper_names_cannot_be_shadowed_and_import_aliases_still_work() {
    support::parity(
        r#"
 import {map as mapped} from "std/fp";
 fn bind(a:i16)->i16{a} fn map(a:i16)->i16{a}
 fn main(){let bind=1;let map=2;println(for {n<-Some(bind);m<-Some(map);yield n+m});println(mapped(Some(3),fn(n:i16)=>n+1));}
 "#,
        "Some(3)\nSome(4)\n",
    );
}

#[test]
fn constructor_polymorphic_and_user_defined_instances_use_the_same_syntax() {
    support::parity(
        r#"
 import {Functor,FlatMap} from "std/fp";
 import {Copy} from "std/value";
 type Boxed<T> = Boxed(T);
 impl Functor for Boxed{fn map<A,B>(value:Self<A>,f:fn(A)->B)->Self<B>{match move(value){Boxed(a)=>Boxed(f(move(a)))}}}
 impl FlatMap for Boxed{fn flat_map<A,B>(value:Self<A>,f:fn(A)->Self<B>)->Self<B>{match move(value){Boxed(a)=>f(move(a))}}}
 fn pair<F<_>:FlatMap+Functor,A:Copy>(first:F<A>,next:fn(A)->F<A>)->F<(A,A)>{for{a<-move(first);b<-next(a);yield (a,b)}}
 fn main(){let a=pair<Option,i16>(Some(20),fn(n:i16)=>Some(n+2));match a{Some((a,b))=>println(a+b),None=>{}};let b=pair<Boxed,i16>(Boxed(20),fn(n:i16)=>Boxed(n+2));match b{Boxed((a,b))=>println(a+b)};}
 "#,
        "42\n42\n",
    );
}

#[test]
fn consuming_continuations_transfer_earlier_and_external_owners() {
    support::parity(
        r#"
 import {Option} from "std/option";
 fn main(){
 let mut drops:i16=0;
 let first=owned(20,fn(n:&mut i16)=>{drops+=1;});
 let second=owned(22,fn(n:&mut i16)=>{drops+=1;});
 let answer=for once {a<-Some(move(first));b<-Some(move(second));yield *a.borrow()+*b.borrow()};
 println(answer);println(drops);
 let unused=owned(9,fn(n:&mut i16)=>{drops+=1;});let empty:Option<i16> = None;
 let skipped=for once {n<-empty;owner<-Some(move(unused));yield *owner.borrow()+n};println(skipped);println(drops);
 }"#,
        "Some(42)\n2\nNone\n3\n",
    );
}

const EFFECTS: &str = r#"
 import {Effect,succeed,run} from "std/effect";
 import {OwnedEffect,succeed as once_value,run as run_once,from_future,ensuring} from "std/effect_once";
 import {Environment} from "std/effect_env";
 import {Future,Poll,Pending,poll_fn} from "std/future";
 import {Context,task_set} from "std/task";
 import {Reactor,block_on} from "std/executor";
 import {Result} from "std/result";
 struct Clock{now:u32}
 impl Reactor for Clock{fn now(&self)->u32{self.now} fn turn(&mut self,delay:Option<u32>)->bool{self.now+=1;true}}
 "#;

#[test]
fn reusable_effect_yield_creates_fresh_owners_only_when_each_run_is_polled() {
    support::parity(
        &format!(
            r#"{EFFECTS}
 fn main(){{
 let mut created:i16=0;let mut dropped:i16=0;
 let program:Effect<unit,String,Owned<i16>> = for<Effect<unit,String,_>> {{
 n<-succeed<unit,String,i16>(42);
 let value={{created+=1;owned(n,fn(n:&mut i16)=>{{dropped+=1;}})}};
 yield move(value)
 }};
 println(created);
 let idle=run(program,{{}});drop(move(idle));println(created);
 let mut clock=Clock{{now:0}};
 for(i in 0..2){{let mut f=run(program,{{}});let value=block_on(&mut f,&mut clock).unwrap().unwrap();println(*value.borrow());drop(move(value));}}
 println(created);println(dropped);
 }}"#
        ),
        "0\n0\n42\n42\n2\n2\n",
    );
}

#[test]
fn consuming_effect_cancellation_releases_child_before_captured_left_and_finalizer() {
    support::parity(
        &format!(
            r#"{EFFECTS}
 fn main(){{
 let first:OwnedEffect<unit,String,Owned<i16>> =once_value(owned(1,fn(n:&mut i16)=>{{println("left");}}));
 let second:OwnedEffect<unit,String,unit> =from_future(once fn(env:Environment<unit>)=>poll_fn(fn(cx:Context)=>{{let pending:Poll<unit> = Pending;pending}},fn()=>{{println("child");}}));
 let program=for once<OwnedEffect<unit,String,_>> {{a<-move(first);_<-move(second);yield *a.borrow()}};
 let program=ensuring(move(program),once fn()=>{{println("finalizer");}});
 let mut f=run_once(move(program),{{}});let set=task_set(1,0).unwrap();let mut cx=Context{{waker:set.allocate().unwrap(),now:0}};
 match f.poll(&mut cx){{Pending=>{{}},_=>{{return;}}}};
 f.cancel();f.cancel();drop(move(f));
 }}"#
        ),
        "child\nleft\nfinalizer\n",
    );
}

#[test]
fn borrowed_or_exclusive_reusable_captures_and_consuming_vectors_are_rejected() {
    reject(
        "fn main(){let value=1;let reference=&value;let p=for{n<-Some(1);yield *reference+n};}",
        "reference",
    );
    reject("fn main(){let owner=owned(1,fn(n:&mut i16)=>{});let p=for{n<-Some(1);m<-Some(2);yield *owner.borrow()+n+m};}", "capture");
    reject(
        "fn main(){let p=for once{n<-[1,2];yield n};}",
        "FunctorOnce",
    );
    reject(
        "fn main(){let p=for{Some(n)<-Some(Some(1));yield n};}",
        "irrefutable",
    );
    reject(
        "fn main(){let wide:u32=70000;let p=for{n:i16<-Some(wide);yield n};}",
        "expected",
    );
    reject(
        "fn main(){let p=for{n<-Some(1);m<-[2];yield n+m};}",
        "expected",
    );
}

#[test]
fn missing_bindings_yields_separators_filters_and_cross_boundary_control_are_explicit() {
    for (body, detail) in [
        ("for {}", "final `yield"),
        ("for {yield 1}", "binding before yield"),
        ("for {let n=1;yield n}", "after the first"),
        ("for {n<-Some(1);}", "final `yield"),
        ("for {n<-Some(1) yield n}", "';'"),
        ("for {n< -Some(1);yield n}", "arrow"),
        ("for {n<-Some(1);if(n>0);yield n}", "guards"),
        ("for {n<-Some(1);yield n;yield 2}", "after final yield"),
        ("for {n<-Some(1);yield {return n;}}", "control flow"),
        ("for {n<-Some(1);yield Some(n)?}", "control flow"),
        ("for {n<-Some(1);yield value.await}", "control flow"),
        ("for {n<-Some(1);yield {break;}}", "control flow"),
    ] {
        reject(&format!("fn main(){{let value={body};}}"), detail);
    }
}

#[test]
fn local_loops_and_explicit_callbacks_keep_their_control_flow() {
    support::parity(
        r#"fn main(){println(for{n<-Some(40);let add=fn(x:i16)=>{if(x==0){return 2;}0};yield {let mut value=n;while(true){value+=add(0);break;}value}});}"#,
        "Some(42)\n",
    );
}

#[test]
fn diagnostics_point_to_the_original_yield_expression() {
    let source = "fn main(){\n let p=for{\n n<-Some(1);\n yield missing+n\n };\n}";
    let error = reject(source, "missing");
    assert_eq!(error.offset, Some(source.find("missing").unwrap()));
    assert_eq!(error.line, 4);
}

#[test]
fn formatting_preserves_arrows_comments_tokens_and_idempotence() {
    let source="fn main(){let x=for once{n<-Some(1);// binding\nlet m=n+1;yield m};let mut n=0;for(i in [1,2]){if(i< -1){n+=i;}}}";
    let formatted = rfl::format_source(source).unwrap();
    assert!(
        formatted.contains("n <- Some(1); // binding"),
        "{formatted}"
    );
    assert!(formatted.contains("for once {\n"), "{formatted}");
    assert!(formatted.contains("i < -1"), "{formatted}");
    assert_eq!(rfl::format_source(&formatted).unwrap(), formatted);
    rfl::check_source(&formatted).unwrap();
    let source = "fn main(){let x=for once<Result<_,String>>{n:i16<-Ok(1);yield n+1};}";
    let formatted = rfl::format_source(source).unwrap();
    assert!(
        formatted.contains("for once<Result<_, String>> {"),
        "{formatted}"
    );
    assert!(formatted.contains("n: i16 <- Ok(1);"), "{formatted}");
    assert_eq!(rfl::format_source(&formatted).unwrap(), formatted);
    rfl::check_source(&formatted).unwrap();
}

#[test]
fn standalone_source_api_also_resolves_compiler_only_fp_dependencies() {
    let source = include_str!("fixtures/comprehension-basic.rfl");
    rfl::check_source(source).unwrap();
    let result = rfl::run_program(source).unwrap();
    assert_eq!(result.status, 0);
    assert_eq!(result.stdout, "Some(42)\n");
}

#[test]
fn bounded_comprehension_executes_on_the_original_brainfuck_cpu() {
    support::brainfuck("fn main(){match for once {a<-Some(40);b<-Some(2);yield a+b}{Some(42)=>putc(79),_=>{}};putc(75);}","OK");
}

#[test]
fn sugar_keeps_the_guest_work_of_equivalent_explicit_combinators() {
    let sugar = "fn main(){println(for{a<-Some(20);let n=a+1;b<-Some(21);yield n+b});}";
    let manual = "import {bind,map} from \"std/fp\";fn main(){println(bind(Some(20),fn(a)=>{let n=a+1;map(Some(21),fn(b)=>n+b)}));}";
    let direct = support::measured(manual, "Some(42)\n", 5_000_000);
    let expanded = support::measured(sugar, "Some(42)\n", 5_000_000);
    assert!(
        expanded <= direct + direct / 20 + 1000,
        "sugar overhead: {direct} -> {expanded}"
    );
}

#[test]
fn one_reusable_callback_can_move_its_input_and_consuming_results_transfer_owned_errors() {
    support::parity(
        r#"
 fn main(){
 let mut dropped:i16=0;
 let value=for{owner<-Some(owned(42,fn(n:&mut i16)=>{dropped+=1;}));yield *owner.borrow()};println(value);println(dropped);
 let failure:Result<i16,Owned<i16>> = Err(owned(9,fn(n:&mut i16)=>{dropped+=1;}));
 let value=for once<Result<_,Owned<i16>>>{n<-move(failure);m<-Ok(n+1);yield m};
 match move(value){Err(error)=>{println(*error.borrow());drop(move(error));},Ok(n)=>{println(n);}};println(dropped);
 }"#,
        "Some(42)\n1\n9\n2\n",
    );
}
