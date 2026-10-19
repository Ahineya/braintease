fn reject(source: &str, message: &str) {
    let error = rfl::check_source(source).expect_err(source);
    assert!(
        error.message.contains(message),
        "expected {message}: {error}"
    );
}

const CONSTRUCTORS: &str = r#"
    trait Functor<_> {
        fn map<A,B>(value:Self<A>, f:fn(A)->B)->Self<B>;
    }
    impl Functor for Option {
        fn map<A,B>(value:Self<A>, f:fn(A)->B)->Self<B> {
            match move(value) {Some(a)=>Some(f(move(a))),None=>None}
        }
    }
    fn mapped<F<_>:Functor,A,B>(value:F<A>, f:fn(A)->B)->F<B> {
        F::map(move(value),f)
    }
    fn main() {println(mapped(Some(41),fn(x:i16)=>x+1));}
"#;

#[test]
fn constructor_bounds_check_and_lower() {
    rfl::check_source(CONSTRUCTORS).unwrap();
}

#[test]
fn constructor_traits_execute_on_native_and_ripple() {
    for result in [
        rfl::run_native_program(CONSTRUCTORS).unwrap(),
        rfl::run_program(CONSTRUCTORS).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "Some(42)\n");
    }
}

#[test]
fn generic_receiver_methods_have_universal_contracts() {
    let source = r#"
        trait Identity {fn identity<T>(&self,value:T)->T;}
        impl Identity for bool {fn identity<A>(&self,value:A)->A {move(value)}}
        fn use_it<I:Identity,T>(instance:I,value:T)->T {instance.identity(move(value))}
        fn main() {println(use_it(true,"generic"));let instance=false;println(Identity::identity(&instance,42));}
    "#;
    for result in [
        rfl::run_native_program(source).unwrap(),
        rfl::run_program(source).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "generic\n42\n");
    }
}

#[test]
fn method_contracts_reject_strengthening_and_mismatched_signatures() {
    reject("trait Identity {fn id<T>(&self,x:T)->T;} impl Identity for bool {fn id<T:Printable>(&self,x:T)->T{x}}", "bounds");
    reject("trait Identity {fn id<T>(&self,x:T)->T;} impl Identity for bool {fn id<T>(&self,x:T)->i16{42}}", "return type");
    reject("trait Identity {fn id<T>(&self,x:T)->T;} impl Identity for bool {fn id<T>(&self,x:i16)->T{x}}", "signature");
    reject("trait Identity {fn id<T>(&self,x:T)->T;} impl Identity for bool {fn id(&self,x:i16)->i16{x}}", "signature");
    reject("trait Identity {fn id<T>(&self,x:T)->T;} impl Identity for bool {fn id<T>(&self,x:T)->T{42}}", "expected");
}

#[test]
fn generic_fp_library_runs_for_core_and_user_constructors() {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/functors.rfl");
    for result in [
        rfl::run_native_file(&path, false).unwrap(),
        rfl::run_file(&path).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(
            result.stdout,
            "Some(42)\n[20, 22]\n[1, 2, 3, 4]\nSome(injected)\n[40, 42]\n42\n"
        );
    }
}

#[test]
fn constructor_contract_errors_cover_kinds_instances_and_ambiguity() {
    let contract = "trait Functor<_>{fn map<A,B>(x:Self<A>,f:fn(A)->B)->Self<B>;}";
    reject(
        &format!("{contract} fn bad<F:Functor>(x:F){{}}"),
        "kind arity",
    );
    reject(&format!("{contract} impl Functor for Option<i16>{{fn map<A,B>(x:Option<A>,f:fn(A)->B)->Option<B>{{None}}}}"),"constructor");
    reject(&format!("{contract} fn f<F<_>:Functor,A>(x:F<A>)->F<A>{{F::map(move(x),fn(a:A)=>move(a))}} fn main(){{f([1]);}}"),"does not implement");
    reject("trait C<_>{fn bad(&self);}", "applied values");
    reject(
        "trait C<_>{type Item;}",
        "associated types on constructor traits",
    );
    reject("trait C<T>{}", "trait parameters");
    reject("trait A<_>{fn op<T>(x:Self<T>)->Self<T>;} trait B<_>{fn op<T>(x:Self<T>)->Self<T>;} fn bad<F<_>:A+B,T>(x:F<T>)->F<T>{F::op(move(x))}","ambiguous");
    reject(
        "trait C<_>{fn op<A>(x:Self<A>)->Self<A>;} fn bad<F<_>,T>(x:F<T>)->F<T>{F::op(move(x))}",
        "bounds or visible traits",
    );
    reject(
        "trait C<_>{} impl C for Option{} impl C for Option{}",
        "overlapping",
    );
    reject("trait C<_,_>{} fn f<F<_>:C>(){}", "kind arity");
}

#[test]
fn generic_method_bounds_outputs_and_impl_parameters_remain_distinct() {
    let source = r#"
        trait Convert {type Output; fn convert<T:Printable>(&self,x:T)->Self::Output;}
        type Boxed<A> = Boxed(A);
        impl<A:Printable> Convert for Boxed<A> {
            type Output=i16;
            fn convert<B:Printable>(&self,x:B)->Self::Output {println(x); 42}
        }
        fn use_it<C:Convert,T:Printable>(c:C,x:T)->C::Output {c.convert(move(x))}
        fn main() {println(use_it(Boxed("owner"),true));}
    "#;
    for result in [
        rfl::run_native_program(source).unwrap(),
        rfl::run_program(source).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "true\n42\n");
    }
    reject(
        "trait C{fn op<T:Printable>(&self,x:T);} impl C for bool{fn op<T>(&self,x:T){}}",
        "bounds",
    );
    reject("trait C{fn op<T>(&self,x:T);} type Boxed<A> = Boxed(A); impl<A> C for Boxed<A>{fn op<A>(&self,x:A){}}", "shadows");
    reject(
        "trait C{fn op<T>(&self,x:T)->T;} fn bad<C0:C>(c:C0)->i16{c.op(true)}",
        "expected",
    );
}

struct Fixture(std::path::PathBuf);
impl Fixture {
    fn new(source: &str) -> Self {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let dir = std::env::temp_dir().join(format!(
            "rfl-constructor-traits-{}-{stamp}",
            std::process::id()
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
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, expected);
    }
}

#[test]
fn functor_and_monad_laws_have_observable_matching_sides() {
    parity_file(r#"
        import {Functor,Monad,map,pure,flat_map} from "std/fp";
        fn identity<A>(a:A)->A{move(a)}
        fn add(a:i16)->i16{a+1}
        fn twice(a:i16)->i16{a*2}
        fn step(a:i16)->Vec<i16>{[a,a+1]}
        fn next(a:i16)->Vec<i16>{[a*2]}
        fn main() {
            let original=[1,2];
            println(map<Vec,i16,i16>(original,identity));println(original);
            println(map(map(original,add),twice));println(map(original,fn(x:i16)=>twice(add(x))));
            println(flat_map(pure<Vec,i16>(3),step));println(step(3));
            let unit:fn(i16)->Vec<i16> = pure;
            println(flat_map(original,unit));println(original);
            println(flat_map(flat_map(original,step),next));
            println(flat_map(original,fn(x:i16)=>flat_map(step(x),next)));
            println(map<Option,i16,i16>(Some(42),identity));println(Some(42));
            println(flat_map(pure<Option,i16>(42),fn(x:i16)=>Some(x+1)));println(Some(43));
            println(flat_map(Some(42),fn(x:i16)=>pure<Option,i16>(x)));println(Some(42));
        }
    "#,"[1, 2]\n[1, 2]\n[4, 6]\n[4, 6]\n[3, 4]\n[3, 4]\n[1, 2]\n[1, 2]\n[2, 4, 4, 6]\n[2, 4, 4, 6]\nSome(42)\nSome(42)\nSome(43)\nSome(43)\nSome(42)\nSome(42)\n");
}

#[test]
fn fp_preserves_callback_order_cow_short_circuiting_and_resource_ownership() {
    parity_file(
        r#"
        import {Functor,Monad,map,flat_map} from "std/fp";
        fn read(x:Owned<i16>)->i16 {*x.borrow()}
        fn main()->i16 {
            let mut calls:i16=0;
            let original=[1,2,3];
            println(map(original,fn(x:i16)=>{calls=calls*10+x;x+1}));
            println(original);println(calls);
            let nothing:Option<i16> = map<Option,i16,i16>(None,fn(x:i16)=>{calls=99;x});
            println(nothing);println(calls);
            let mut drops:i16=0;
            let input=[owned(20,fn(x:&mut i16)=>{drops+=1;}),owned(22,fn(x:&mut i16)=>{drops+=1;})];
            println(map(move(input),read));
            if (drops!=2) {return 1;}
            let input=Some(owned(42,fn(x:&mut i16)=>{drops+=1;}));
            let output=flat_map(move(input),fn(x:Owned<i16>)=>Some(move(x)));
            drop(move(output));
            if (drops!=3) {return 2;}
            let input=[owned(1,fn(x:&mut i16)=>{drops+=1;}),owned(2,fn(x:&mut i16)=>{drops+=1;})];
            let output=flat_map(move(input),fn(x:Owned<i16>)=>[move(x)]);
            drop(move(output));
            if (drops!=5) {return 3;}
            0
        }
    "#,
        "[2, 3, 4]\n[1, 2, 3]\n123\nNone\n123\n[20, 22]\n",
    );
}

#[test]
fn self_does_not_leak_into_argument_expressions_and_contracts_do_not_reach_ir() {
    reject("trait C<_>{fn op<T>(value:Self<T>,f:fn(T)->T)->Self<T>;} fn bad<F<_>:C,T>(value:F<T>)->F<T>{F::op(move(value),fn(x:Self<T>)=>x)}", "unknown type");
    let ir = rfl::compile_to_ir_text(CONSTRUCTORS).unwrap();
    assert!(
        !ir.contains("$contract$") && !ir.contains("$bound$") && !ir.contains("$param$"),
        "{ir}"
    );
}

#[test]
fn declared_constructor_bound_fixes_dispatch_before_specialization() {
    let source = r#"
        trait First<_>{fn op<A>(x:Self<A>)->Self<A>;}
        trait Second<_>{fn op<A>(x:Self<A>)->Self<A>;}
        impl First for Option {fn op<A>(x:Self<A>)->Self<A>{println(1);move(x)}}
        impl Second for Option {fn op<A>(x:Self<A>)->Self<A>{println(2);move(x)}}
        fn selected<F<_>:First,T>(x:F<T>)->F<T>{F::op(move(x))}
        fn main(){println(selected(Some(42)));println(Second::op(Some(42)));}
    "#;
    for result in [
        rfl::run_native_program(source).unwrap(),
        rfl::run_program(source).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "1\nSome(42)\n2\nSome(42)\n");
    }
}

#[test]
fn constructor_instances_and_generic_bounds_cross_module_aliases_and_obey_orphans() {
    let source = r#"
        import {Functor as Mappable,map as transform} from "std/fp";
        import {Container,Boxed} from "./container";
        fn forwarded<F<_>:Mappable,A,B>(x:F<A>,f:fn(A)->B)->F<B>{transform(move(x),f)}
        fn main(){match forwarded<Container,i16,i16>(Boxed(41),fn(x:i16)=>x+1){Boxed(value)=>println(value)};}
    "#;
    let file = Fixture::new(source);
    std::fs::write(file.0.parent().unwrap().join("container.rfl"),r#"
        import {Functor} from "std/fp";
        export type Container<T> = Boxed(T);
        impl Functor for Container {fn map<A,B>(x:Self<A>,f:fn(A)->B)->Self<B>{match move(x){Boxed(a)=>Boxed(f(move(a)))}}}
    "#).unwrap();
    for result in [
        rfl::run_native_file(&file.0, false).unwrap(),
        rfl::run_file(&file.0).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "42\n");
    }
    std::fs::write(&file.0,r#"import {Functor} from "std/fp"; impl Functor for Owned {fn map<A,B>(x:Self<A>,f:fn(A)->B)->Self<B>{owned(f(move(x).into_inner()),fn(b:&mut B)=>{})}}"#).unwrap();
    let error = rfl::check_file(&file.0).unwrap_err();
    assert!(error.message.contains("orphan"), "{error}");
}

#[test]
fn lambda_results_are_inferred_from_values_and_agree_with_early_returns() {
    let source = r#"
        fn main() {
            let text=fn()=>"text";
            let optional=fn(flag:bool)=>{if(flag){return Some(1);}Some(2)};
            let all_return=fn(flag:bool)=>{if(flag){return "yes";}else{return "no";}};
            let generic=fn(x:i16)=>Some([x]);
            println(text());println(optional(true));println(optional(false));println(all_return(false));println(generic(42));
        }
    "#;
    for result in [
        rfl::run_native_program(source).unwrap(),
        rfl::run_program(source).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "text\nSome(1)\nSome(2)\nno\nSome([42])\n");
    }
    reject(
        "fn main(){let f=fn(flag:bool)=>{if(flag){return \"text\";}Some(1)};}",
        "lambda returns",
    );
    reject(
        "fn main(){let f=fn(flag:bool)=>{if(flag){return \"text\";}};}",
        "lambda returns",
    );
}

#[test]
fn constructor_and_generic_receiver_dispatch_run_on_the_brainfuck_cpu() {
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
        trait Keep<_>{fn keep<T>(x:Self<T>)->Self<T>;}
        impl Keep for Option{fn keep<T>(x:Self<T>)->Self<T>{move(x)}}
        trait Identity{fn id<T>(&self,x:T)->T;}
        impl Identity for bool{fn id<T>(&self,x:T)->T{move(x)}}
        fn keep<F<_>:Keep,T>(x:F<T>)->F<T>{F::keep(move(x))}
        fn main(){let instance=true;match keep(Some(79)){Some(x)=>putc(instance.id(x)),None=>{}};putc(75);}
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
    assert_eq!(String::from_utf8(result.stdout).unwrap(), "OK");
}

#[test]
fn binary_constructor_traits_swap_result_arguments_and_errors_hide_internal_contracts() {
    let source = r#"
        trait Swap<_,_>{fn swap<A,B>(value:Self<A,B>)->Self<B,A>;}
        impl Swap for Result {
            fn swap<A,B>(value:Self<A,B>)->Self<B,A> {
                match move(value){Ok(a)=>Err(move(a)),Err(b)=>Ok(move(b))}
            }
        }
        fn swapped<F<_,_>:Swap,A,B>(value:F<A,B>)->F<B,A>{F::swap(move(value))}
        fn main(){let value:Result<i16,String> = Ok(42);println(swapped(value));}
    "#;
    for result in [
        rfl::run_native_program(&source).unwrap(),
        rfl::run_program(&source).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "Err(42)\n");
    }
    let invalid = CONSTRUCTORS.replace("F::map(move(value),f)", "F::map(move(value))");
    let error = rfl::check_source(&invalid).unwrap_err();
    assert!(
        error.detail.contains("Functor::map") && !error.detail.contains("$contract$"),
        "{error}"
    );
    reject(
        "fn main(){let f=fn(x:Option<i16>)=>{let n=x?;Some(n)};}",
        "expected function signature",
    );
}

#[test]
fn inferred_lambda_results_cannot_smuggle_references_through_products() {
    reject(
        "fn main(){let f=fn()=>{let value=42;(0,&value)};}",
        "cannot return a reference",
    );
    reject(
        "fn main(){let f=fn()=>{let value=42;return (0,(&value,1));};}",
        "cannot return a reference",
    );
    reject(
        "fn main(){let f:fn()->(i16,&i16)=fn()=>{let value=42;(0,&value)};}",
        "cannot return a reference",
    );
}
