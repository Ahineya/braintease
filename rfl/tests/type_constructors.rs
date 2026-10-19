fn reject(source: &str, message: &str) {
    let error =
        rfl::check_source(source).expect_err(&format!("accepted invalid program: {source}"));
    assert!(
        error.message.contains(message),
        "expected {message}: {error}"
    );
}

const BASIC: &str = r#"
    type Boxed<T> = Boxed(T);
    fn same<F<_>, A>(value:F<A>) -> F<A> { move(value) }
    fn forward<G<_>, T>(value:G<T>) -> G<T> { same(move(value)) }
    fn same2<F<_, _>, A, B>(value:F<A,B>) -> F<A,B> { move(value) }
    fn main() {
        println(same<Option, i16>(Some(42)));
        match forward(Boxed("user type")) { Boxed(value) => println(value) };
        println(same([20,22]));
        let result:Result<i16,String> = same2(Ok(42));
        println(result);
    }
"#;

#[test]
fn constructor_parameters_check_and_specialize() {
    rfl::check_source(BASIC).unwrap();
}

#[test]
fn constructor_parameters_execute_on_native_and_ripple() {
    for result in [
        rfl::run_native_program(BASIC).unwrap(),
        rfl::run_program(BASIC).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "Some(42)\nuser type\n[20, 22]\nOk(42)\n");
    }
}

#[test]
fn kind_errors_are_checked_even_for_unused_declarations() {
    reject("fn bad<F<_>>(x:F) {}", "constructor");
    reject("fn bad<F<_>>(x:F<i16,bool>) {}", "expects 1");
    reject("fn bad<F<_>,A>(x:F<A>)->F<A> { Some(1) }", "expected");
    reject("fn bad<F<_>,G<_>,A>(x:F<A>)->G<A> { move(x) }", "expected");
    reject("fn bad<F<_>>(x:Vec<F>) {}", "constructor");
    reject("fn bad<F<_>:Printable>(x:F<i16>) {}", "kind arity");
}

#[test]
fn constructor_arguments_must_have_the_declared_kind() {
    let declaration = "fn same<F<_>, A>(x:F<A>)->F<A> {move(x)}";
    reject(
        &format!("{declaration} fn main() {{ same<i16,i16>(1); }}"),
        "constructor",
    );
    reject(
        &format!("{declaration} fn main() {{ same<Result,i16>(Ok(1)); }}"),
        "constructor",
    );
    reject(
        &format!("{declaration} fn main() {{ same<Option<i16>,i16>(Some(1)); }}"),
        "constructor",
    );
}

#[test]
fn constructor_polymorphic_standard_library_runs_on_both_targets() {
    let path =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/type-constructors.rfl");
    for result in [
        rfl::run_native_file(&path, false).unwrap(),
        rfl::run_file(&path).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(
            result.stdout,
            "Some(42)\n[20, 22]\n42\nSome(42)\nNone\n[42]\n"
        );
    }
}

#[test]
fn constructor_parameters_in_data_types_and_nested_applications() {
    let source = r#"
        type Wrap<F<_>, A> = Wrap(F<A>);
        type Boxed<T> = Boxed(T);
        fn nested<F<_>, A>(value:F<F<A>>) -> F<F<A>> {move(value)}
        fn unwrap<F<_>, A>(value:Wrap<F,A>) -> F<A> {
            match move(value) {Wrap(value)=>move(value)}
        }
        fn main() {
            let wrapped:Wrap<Option,i16> = Wrap(Some(42));
            println(unwrap(move(wrapped)));
            println(unwrap(Wrap([20,22])));
            println(nested(Some(Some(42))));
            println(nested([[42]]));
            let typed:Wrap<Boxed, String> = Wrap(Boxed("managed"));
            match unwrap(move(typed)) {Boxed(text)=>println(text)};
        }
    "#;
    for result in [
        rfl::run_native_program(source).unwrap(),
        rfl::run_program(source).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(
            result.stdout,
            "Some(42)\n[20, 22]\nSome(Some(42))\n[[42]]\nmanaged\n"
        );
    }
}

#[test]
fn constructor_inference_checks_identity_and_payload_consistency() {
    let pair = "fn pair<F<_>, A>(a:F<A>,b:F<A>)->F<A> {drop(move(a));move(b)}";
    reject(
        &format!("{pair} fn main() {{pair(Some(1),[2]);}}"),
        "expected",
    );
    reject(
        &format!("{pair} fn main() {{pair(Some(1),Some(true));}}"),
        "expected",
    );
    reject("fn bad<F<_>,A>(x:F<F<A>>)->F<A> {move(x)}", "expected");
    reject("fn bad<A>(x:A<i16>) {}", "does not take arguments");
    reject("type Bad<F<_>> = Bad(F);", "constructor");
    reject("type Bad<F<_>> = Bad(Vec<F>);", "constructor");
    reject(
        "type Wrap<F<_>,A> = Wrap(F<A>); fn f(x:Wrap<i16,i16>) {}",
        "constructor",
    );
    reject(
        "type Wrap<F<_>,A> = Wrap(F<A>); fn f(x:Wrap<Result,i16>) {}",
        "constructor",
    );
}

#[test]
fn unsupported_higher_kind_forms_have_explicit_diagnostics() {
    reject("fn f<F<T>>(x:i16) {}", "`_` slots");
    reject("fn f<F<_<_>>>(x:i16) {}", "expected");
    reject(
        "impl<F<_>> Option<F<i16>> {fn f(self) {}}",
        "constructor parameters in impl",
    );
    reject("type Wrap<F<_>,A> = Wrap(F<A>); fn f<G<_>, A>(x:G<A>)->G<A> {move(x)} fn main() {f<Wrap,i16>(1);}", "constructor");
    reject(
        "fn same<F<_>,A>(x:F<A>)->F<A> {move(x)} fn inferred(x) {same(x)}",
        "add parameter types",
    );
}

#[test]
fn formatter_preserves_constructor_parameter_syntax() {
    let source = "fn same<F<_>,A>(x:F<A>)->F<A>{move(x)}";
    let formatted = rfl::format_source(source).unwrap();
    assert!(formatted.contains("same<F<_>, A>"), "{formatted}");
    assert_eq!(rfl::format_source(&formatted).unwrap(), formatted);
    rfl::check_source(&formatted).unwrap();
}

#[test]
fn applied_constructors_preserve_resource_moves_and_cleanup() {
    let source = r#"
        type Wrap<F<_>,A> = Wrap(F<A>);
        fn same<F<_>,A>(x:F<A>)->F<A> {move(x)}
        fn unwrap<F<_>,A>(x:Wrap<F,A>)->F<A> {match move(x) {Wrap(x)=>move(x)}}
        fn main()->i16 {
            let mut drops:i16=0;
            {
                let first=owned(42,fn(x:&mut i16)=>{drops+=1;});
                let second=same(move(first));
                println(*second.borrow());
                let wrapped:Wrap<Option,Owned<String>> = Wrap(Some(owned("owned",fn(x:&mut String)=>{drops+=1;})));
                match unwrap(move(wrapped)) {Some(value)=>println(*value.borrow()),None=>{return 1;}};
                if (drops!=1) {return 2;}
            }
            if (drops!=2) {return 3;}
            0
        }
    "#;
    for result in [
        rfl::run_native_program(source).unwrap(),
        rfl::run_program(source).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "42\nowned\n");
    }
    reject(
        "fn copy<F<_>,A>(x:F<A>)->F<A>{x} fn main(){copy(owned(1,fn(x:&mut i16)=>{}));}",
        "move",
    );
    reject("fn same<F<_>,A>(x:F<A>)->F<A>{move(x)} fn main(){let a=owned(1,fn(x:&mut i16)=>{}); same(move(a)); a.borrow();}", "moved value");
    reject(
        "fn same<F<_>,A>(x:F<A>)->F<A>{move(x)} fn main(){let a=1; same<Option,&i16>(Some(&a));}",
        "reference cannot be stored",
    );
}

#[test]
fn constructor_type_arguments_do_not_become_runtime_parameters() {
    let source = "fn same<F<_>,A>(x:F<A>)->F<A>{move(x)} fn main(){println(same(Some(42)));}";
    let ir = rfl::compile_to_ir_text(source).unwrap();
    // The implementation specializes to Option<i16>, with no constructor dispatch.
    assert!(ir.contains("same$constructor$"), "{ir}");
    assert!(!ir.contains("$param$"), "rigid declarations escaped: {ir}");
}

#[test]
fn contextual_function_values_specialize_and_check_all_bounds() {
    let source = r#"
        fn identity<A>(value:A)->A {move(value)}
        fn same<F<_>,A>(value:F<A>)->F<A> {move(value)}
        fn main() {
            let id:fn(i16)->i16 = identity;
            let forward:fn(Option<i16>)->Option<i16> = same;
            println(id(42));println(forward(Some(42)));
        }
    "#;
    for result in [
        rfl::run_native_program(source).unwrap(),
        rfl::run_program(source).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "42\nSome(42)\n");
    }
    reject("fn same<F<_>,A>(value:F<A>)->F<A>{move(value)} fn main(){let f:fn(Option<i16>)->Vec<i16> = same;}","expected");
    reject(
        "fn unused<T>(n:i16)->i16{n} fn main(){let f:fn(i16)->i16=unused;}",
        "cannot infer",
    );
    reject("struct Hidden {n:i16} fn show<T:Printable>(value:T){println(value);} fn main(){let f:fn(Hidden)->unit=show;}","does not implement");
    reject(
        "fn main(){let a=1;let value=Some(&a);}",
        "reference cannot be stored",
    );
}

#[test]
fn slices_keep_cow_snapshots_and_constructor_arity_is_not_hardcoded() {
    let source = r#"
        type Triple<A,B,C> = Triple(A,B,C);
        fn same<F<_>,A>(x:F<A>)->F<A>{move(x)}
        fn forward<G<_>,T>(x:G<T>)->G<T>{same<G,T>(move(x))}
        fn same3<F<_,_,_>,A,B,C>(x:F<A,B,C>)->F<A,B,C>{move(x)}
        fn main() {
            let mut values=[10,20,30];
            let view=forward<Slice,i16>(values[1..3]);
            values[1]=99;
            println(view[0]);println(values[1]);
            match same3(Triple("managed",[42],true)) {
                Triple(text,numbers,flag)=>{println(text);println(numbers);println(flag);}
            };
        }
    "#;
    for result in [
        rfl::run_native_program(source).unwrap(),
        rfl::run_program(source).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "20\n99\nmanaged\n[42]\ntrue\n");
    }
}
