fn reject(source: &str, message: &str) {
    let error = rfl::check_source(source).expect_err(source);
    assert!(
        error.message.contains(message),
        "expected {message}: {error}"
    );
}

const BASIC: &str = r#"
    trait Keep<_> {fn keep<A>(value:Self<A>)->Self<A>;}
    impl<E> Keep for Result<_, E> {
        fn keep<A>(value:Self<A>)->Self<A> {move(value)}
    }
    fn same<F<_>:Keep,A>(value:F<A>)->F<A> {F::keep(move(value))}
    fn forward<E,A>(value:Result<A,E>)->Result<A,E> {
        same<Result<_,E>,A>(move(value))
    }
    fn main() {
        let value:Result<i16,String> = Ok(42);
        println(forward(value));
        println(same<Result<_,String>,i16>(Err("failed")));
    }
"#;

#[test]
fn partial_constructors_apply_and_dispatch_with_generic_fixed_arguments() {
    rfl::check_source(BASIC).unwrap();
}

#[test]
fn partial_constructor_dispatch_executes_on_native_and_ripple() {
    for result in [
        rfl::run_native_program(BASIC).unwrap(),
        rfl::run_program(BASIC).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "Ok(42)\nErr(failed)\n");
    }
}

#[test]
fn partial_constructor_kinds_are_checked_before_specializing() {
    let f = "fn same<F<_>,A>(value:F<A>)->F<A>{move(value)}";
    reject(
        &format!("{f} fn main(){{same<Result<_,_>,i16>(Ok<i16,String>(42));}}"),
        "constructor",
    );
    reject(
        &format!("{f} fn main(){{same<Result<_,String,bool>,i16>(Ok<i16,String>(42));}}"),
        "expects 2",
    );
    reject("fn bad(value:Result<_,String>){}", "placeholder");
    reject("fn bad<_>(){}", "reserved");
    reject("type Bad<_> = Bad(i16);", "reserved");
    reject("trait C<_,_>{} impl<_> C for Result<_,_>{}", "reserved");
    reject("fn bad<F<_>>(value:F<i16>){} fn main(){bad<Result<Vec<_>,String>>(Ok<Vec<i16>,String>([42]));}", "placeholder");
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

struct Fixture(std::path::PathBuf);
impl Fixture {
    fn new(source: &str) -> Self {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let dir = std::env::temp_dir().join(format!("rfl-partials-{}-{stamp}", std::process::id()));
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
fn slots_fill_left_to_right_and_partial_application_composes() {
    parity(
        r#"
        type Triple<A,B,C> = Triple(A,B,C);
        fn same<F<_>,A>(x:F<A>)->F<A>{move(x)}
        fn same2<F<_,_>,A,B>(x:F<A,B>)->F<A,B>{move(x)}
        fn fixed<F<_,_>,E,A>(x:F<A,E>)->F<A,E>{same<F<_,E>,A>(move(x))}
        fn fixed_twice<F<_,_,_>,A,B>(x:F<A,String,B>)->F<A,String,B> {
            fixed<F<_,String,_>,B,A>(move(x))
        }
        fn main() {
            let v:Result<i16,String> = Ok<_,_>(42);
            println(fixed<Result,_,_>(v));
            println(same<Result<_,String>,_>(v));
            println(same2<Result<_,_>,_,_>(v));
            println(same<Result<i16,_>,String>(Err("error slot")));
            match same2<Triple<_,String,_>,_,_>(Triple(42,"middle",true)) {
                Triple(a,b,c)=>{println(a);println(b);println(c);}
            };
            match fixed_twice<Triple,_,_>(Triple(21,"composed",false)) {
                Triple(a,b,c)=>{println(a);println(b);println(c);}
            };
        }
    "#,
        "Ok(42)\nOk(42)\nOk(42)\nErr(error slot)\n42\nmiddle\ntrue\n21\ncomposed\nfalse\n",
    );
}

#[test]
fn constructors_in_data_preserve_fixed_arguments_and_canonical_identity() {
    parity(
        r#"
        type Wrap<F<_>,A> = Wrap(F<A>);
        fn unwrap<E,A>(x:Wrap<Result<_,E>,A>)->Result<A,E>{match move(x){Wrap(x)=>move(x)}}
        fn identity<F<_>,A>(x:Wrap<F,A>)->Wrap<F,A>{move(x)}
        fn generic_unwrap<G<_,_>,E,A>(x:Wrap<G<_,E>,A>)->G<A,E>{match move(x){Wrap(x)=>move(x)}}
        fn main(){
            let first:Wrap<Result<_,String>,i16> = Wrap(Ok(42));
            let second:Wrap<Result<_,String>,i16> = identity(move(first));
            println(unwrap(move(second)));
            let error:Wrap<Result<_,String>,bool> = Wrap(Err("retained"));
            println(unwrap(move(error)));
            let generic:Wrap<Result<_,bool>,i16> = Wrap(Ok(21));
            println(generic_unwrap<Result,_,_>(move(generic)));
        }
    "#,
        "Ok(42)\nErr(retained)\nOk(21)\n",
    );
}

#[test]
fn partial_impl_coherence_checks_slots_and_all_open_normalization() {
    let contract = "trait Keep<_>{fn keep<A>(x:Self<A>)->Self<A>;}";
    reject(&format!("{contract} impl<E> Keep for Result<_,E>{{fn keep<A>(x:Self<A>)->Self<A>{{move(x)}}}} impl Keep for Result<_,String>{{fn keep<A>(x:Self<A>)->Self<A>{{move(x)}}}}"), "overlapping");
    reject(
        "trait C<_,_>{} impl C for Result{} impl C for Result<_,_>{}",
        "overlapping",
    );
    reject(
        "trait C<_>{} impl<E> C for Result<_,String>{}",
        "unconstrained",
    );
    rfl::check_source(&format!("{contract} impl<E> Keep for Result<_,E>{{fn keep<A>(x:Self<A>)->Self<A>{{move(x)}}}} impl<T> Keep for Result<T,_>{{fn keep<A>(x:Self<A>)->Self<A>{{move(x)}}}}" )).unwrap();
    parity(
        r#"
        trait Side<_>{fn side<A>(x:Self<A>)->i16;}
        impl<E> Side for Result<_,E>{fn side<A>(x:Self<A>)->i16{drop(move(x));1}}
        impl<T> Side for Result<T,_>{fn side<A>(x:Self<A>)->i16{drop(move(x));2}}
        fn side<F<_>:Side,A>(x:F<A>)->i16{F::side(move(x))}
        fn main(){
            let value:Result<i16,bool> = Ok(42);
            println(side<Result<_,bool>,_>(value));
            println(side<Result<i16,_>,_>(value));
        }
    "#,
        "1\n2\n",
    );
}

#[test]
fn inference_never_guesses_the_open_slot_or_changes_a_fixed_type() {
    let f = "fn same<F<_>,A>(x:F<A>)->F<A>{move(x)}";
    reject(
        &format!("{f} fn main(){{same(Ok<i16,String>(42));}}"),
        "explicit slots",
    );
    reject(&format!("{f} fn main(){{same(Ok(42));}}"), "explicit slots");
    reject(
        &format!("{f} fn main(){{same<Result<_,bool>,_>(Ok<i16,String>(42));}}"),
        "expected",
    );
    reject(
        "fn unused<T>()->i16{42} fn main(){unused<_>();}",
        "cannot infer",
    );
    reject(
        "fn main(){let x:Result<i16,String> = Ok<bool,_>(true);}",
        "expected",
    );
    reject(
        "fn main(){let x:Result<i16,String> = Err<_,bool>(false);}",
        "expected",
    );
    reject(
        "type Bad<F<_>> = Bad(F<i16>); fn bad(value:Bad<Result<_,Missing>>){}",
        "unknown type",
    );
    reject("trait C<_>{fn op<A>(x:Self<A>)->Self<A>;} impl<E> C for Result<_,E>{fn op<A>(x:Self<A>)->Self<A>{Ok<i16,E>(42)}}", "expected");
}

#[test]
fn known_fixed_arguments_contextualize_inline_variants_once() {
    parity(
        r#"
        type Wrap<F<_>,A> = Wrap(F<A>);
        fn same<F<_>,A>(x:F<A>)->F<A>{move(x)}
        fn keep<A,E>(x:Result<A,E>)->Result<A,E>{move(x)}
        fn unpack<F<_>,A>(x:Wrap<F,A>)->F<A>{match move(x){Wrap(x)=>move(x)}}
        fn main(){
            let mut calls=0;
            println(same<Result<_,String>,_>(Ok({calls+=1;42})));
            println(keep<_,String>(Ok<_,_>(21)));
            println(unpack<Result<_,String>,_>(Wrap(Ok({calls+=1;43}))));
            println(calls);
        }
    "#,
        "Ok(42)\nOk(21)\nOk(43)\n2\n",
    );
    reject("fn keep<A,E>(x:Result<A,E>)->Result<A,E>{move(x)} fn main(){keep<_,String>(Ok<i16,bool>(42));}","expected");
    reject("fn keep<A,E>(x:Result<A,E>)->Result<A,E>{move(x)} fn main(){keep<_,String>(Ok<_,_,bool>(42));}","type arguments");
    reject("fn same<F<_>,A>(x:F<A>)->F<A>{move(x)} fn main(){let x=42;same<Result<_,String>,_>(Ok(&x));}","reference cannot be stored");
}

#[test]
fn fixed_arguments_respect_bounds_even_without_constructing_values() {
    reject("trait Mark{} type Bounded<A,E:Mark> = Bounded(A,E); type Wrap<F<_>> = Wrap(i16); fn bad(x:Wrap<Bounded<_,bool>>){}", "does not implement");
    reject("trait Mark{} type Bounded<A:Mark,E> = Bounded(A,E); trait C<_>{fn op<A>(x:Self<A>)->Self<A>;} impl<E> C for Bounded<_,E>{fn op<A>(x:Self<A>)->Self<A>{move(x)}}", "does not implement");
}

#[test]
fn result_standard_library_example_executes_on_both_targets() {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/fallible-fp.rfl");
    for result in [
        rfl::run_native_file(&path, false).unwrap(),
        rfl::run_file(&path).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(
            result.stdout,
            "Ok(42)\nOk(mapped)\nOk(42)\nErr(negative input)\n"
        );
    }
}

#[test]
fn result_fp_obeys_laws_and_short_circuits_in_order() {
    parity_file(r#"
        import {Functor,Monad,map,pure,flat_map,map_result,flat_map_result} from "std/fp";
        fn step(x:i16)->Result<i16,String>{if(x>0){Ok(x+1)}else{Err("negative")}}
        fn next(x:i16)->Result<i16,String>{Ok(x*2)}
        fn inject(x:i16)->Result<i16,String>{pure<Result<_,String>,_>(x)}
        fn main(){
            let original:Result<i16,String> = Ok(20);
            println(map_result(original,fn(x:i16)=>x));println(original);
            println(map_result(map_result(original,fn(x:i16)=>x+1),fn(x:i16)=>x*2));
            println(map_result(original,fn(x:i16)=>(x+1)*2));
            println(flat_map_result(inject(20),step));println(step(20));
            println(flat_map_result(original,inject));println(original);
            println(flat_map_result(flat_map_result(original,step),next));
            println(flat_map_result(original,fn(x:i16)=>flat_map_result(step(x),next)));
            let mut calls=0;
            let failure:Result<i16,String> = Err("stop");
            println(map<Result<_,String>,_,_>(failure,fn(x:i16)=>{calls+=1;x+1}));
            println(flat_map<Result<_,String>,_,_>(failure,fn(x:i16)=>{calls+=1;next(x)}));
            println(calls);
        }
    "#, "Ok(20)\nOk(20)\nOk(42)\nOk(42)\nOk(21)\nOk(21)\nOk(20)\nOk(20)\nOk(42)\nOk(42)\nErr(stop)\nErr(stop)\n0\n");
}

#[test]
fn result_fp_transfers_success_and_failure_resources_once() {
    parity_file(
        r#"
        import {Functor,Monad,map,flat_map,map_result,flat_map_result} from "std/fp";
        fn main()->i16 {
            let mut drops=0;
            let mut calls=0;
            {
                let input:Result<Owned<i16>,String> = Ok(owned(42,fn(x:&mut i16)=>{drops+=1;}));
                let output:Result<Owned<i16>,String> = flat_map_result(move(input),fn(x:Owned<i16>)=>Ok(move(x)));
                match move(output){Ok(x)=>println(*x.borrow()),Err(_)=>{return 1;}};
                if(drops!=1){return 2;}
                let input:Result<i16,Owned<String>> = Err(owned("owned error",fn(x:&mut String)=>{drops+=1;}));
                let output=map<Result<_,Owned<String>>,_,_>(move(input),fn(x:i16)=>{calls+=1;x+1});
                let output:Result<i16,Owned<String>> = flat_map<Result<_,Owned<String>>,_,_>(move(output),fn(x:i16)=>{calls+=1;Ok(x)});
                if(drops!=1||calls!=0){return 3;}
                match move(output){Ok(_)=>{return 4;},Err(error)=>println(*error.borrow())};
            }
            if(drops!=2){return 5;}
            println(drops);
            0
        }
    "#,
        "42\nowned error\n2\n",
    );
    reject("fn same<F<_>,A>(x:F<A>)->F<A>{move(x)} fn main(){let a=1;same<Result<_,&i16>,i16>(Err(&a));}", "reference cannot be stored");
}

#[test]
fn partial_constructors_are_formatter_stable_and_do_not_reach_ir() {
    let formatted = rfl::format_source(BASIC).unwrap();
    assert!(formatted.contains("Result<_, E>"), "{formatted}");
    assert_eq!(rfl::format_source(&formatted).unwrap(), formatted);
    rfl::check_source(&formatted).unwrap();
    let ir = rfl::compile_to_ir_text(BASIC).unwrap();
    assert!(
        !ir.contains("$contract$") && !ir.contains("$param$"),
        "{ir}"
    );
}

#[test]
fn partial_instances_follow_import_aliases_and_cannot_bypass_orphans() {
    let file = Fixture::new(
        r#"
        import {Carrier as Renamed, Payload} from "./model";
        trait Keep<_>{fn keep<A>(x:Self<A>)->Self<A>;}
        impl<E> Keep for Renamed<_,E>{fn keep<A>(x:Self<A>)->Self<A>{move(x)}}
        fn keep<F<_>:Keep,A>(x:F<A>)->F<A>{F::keep(move(x))}
        fn main(){match keep<Renamed<_,String>,_>(Payload(42,"alias")){Payload(n,s)=>{println(n);println(s);}};}
    "#,
    );
    std::fs::write(
        file.0.with_file_name("model.rfl"),
        "export type Carrier<A,E> = Payload(A,E);",
    )
    .unwrap();
    for result in [
        rfl::run_native_file(&file.0, false).unwrap(),
        rfl::run_file(&file.0).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "42\nalias\n");
    }
    std::fs::write(&file.0,r#"import {Functor} from "std/fp"; impl<E> Functor for Result<_,E>{fn map<A,B>(x:Self<A>,f:fn(A)->B)->Self<B>{match move(x){Ok(a)=>Ok(f(move(a))),Err(e)=>Err(move(e))}}}"#).unwrap();
    assert!(rfl::check_file(&file.0)
        .unwrap_err()
        .message
        .contains("orphan"));
}

#[test]
fn partial_constructor_dispatch_runs_on_the_brainfuck_cpu() {
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
        impl<E> Keep for Result<_,E>{fn keep<T>(x:Self<T>)->Self<T>{move(x)}}
        fn keep<F<_>:Keep,T>(x:F<T>)->F<T>{F::keep(move(x))}
        fn main(){
            match keep<Result<_,bool>,_>(Ok(79)){Ok(x)=>putc(x),Err(_)=>{}};
            match keep<Result<_,i16>,bool>(Err(75)){Ok(_)=>{},Err(x)=>putc(x)};
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
    assert_eq!(String::from_utf8(result.stdout).unwrap(), "OK");
}
