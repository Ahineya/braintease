fn check(source: &str) {
    rfl::check_source(source).unwrap_or_else(|err| panic!("{err}"));
}
fn reject(source: &str, message: &str) {
    let error = rfl::check_source(source).expect_err("accepted invalid associated type");
    assert!(
        error.message.contains(message),
        "expected {message}: {error}"
    );
}

const BASIC: &str = r#"
    trait Produce { type Output; fn produce(&self) -> Self::Output; }
    struct Count { value: i16 }
    impl Produce for Count { type Output = i16; fn produce(&self) -> Self::Output { self.value } }
    fn produce<T: Produce>(value: T) -> T::Output { value.produce() }
    fn main() -> i16 { produce(Count { value: 42 }) }
"#;

#[test]
fn associated_output_in_trait_impl_and_generic_signature() {
    check(BASIC);
}
#[test]
fn associated_output_is_concrete_on_native_and_ripple() {
    assert_eq!(rfl::run_native_program(BASIC).unwrap().status, 42);
    assert_eq!(rfl::run_program(BASIC).unwrap().status, 42);
}
#[test]
fn associated_output_errors_are_explicit() {
    reject(
        "trait A { type Output; } impl A for i16 {}",
        "missing associated type",
    );
    reject(
        "trait A { type Output; } impl A for i16 { type Output = Self::Output; }",
        "cycle",
    );
}

#[test]
fn associated_bounds_work_in_generic_bodies_and_are_checked_for_every_impl() {
    check(
        r#"
        trait Produce { type Output: Printable; fn produce(&self) -> Self::Output; }
        impl Produce for i16 { type Output = String; fn produce(&self) -> String { "hello" } }
        fn show<T: Produce>(x: T) { x.produce().print(); }
        fn main() { show(1); }
    "#,
    );
    reject("trait A { type Out: Missing; }", "unknown trait");
    reject(
        "trait A { type Out: Printable + Printable; }",
        "duplicate bound",
    );
    reject("struct Hidden { n:i16 } trait A { type Out: Printable; } impl A for i16 { type Out = Hidden; }", "does not implement");
    reject(
        "trait A { type Out: Printable; } impl<T> A for Vec<T> { type Out = T; }",
        "does not implement",
    );
    check("trait A { type Out: Printable; } impl<T: Printable> A for Vec<T> { type Out = T; } fn main() {}");
}

#[test]
fn generic_contracts_do_not_guess_the_eventual_associated_type() {
    reject(
        "trait A { type Out; } fn bad<T: A>(x:T) -> T::Out { 42 } fn main() {}",
        "expected",
    );
    reject(
        "trait A { type Out; } fn bad<T: A>(x:T, y:T::Out) { y.print(); }",
        "does not implement",
    );
    reject(
        "trait A { type Out; } fn bad<T>(x:T) -> T::Out { x }",
        "associated type",
    );
    reject(
        "trait A { type Out; } fn bad<T: A>(x:T) -> T::Missing { x }",
        "associated type",
    );
    reject("trait A { type Out; } impl A for i16 { type Out = bool; } fn bad<T:A>(x:T) -> T::Out { 42 } fn main() { bad(1); }", "expected");
    reject("fn bad(x:Self) {}", "unknown type");
}

#[test]
fn associated_signature_mismatches_and_unused_values_are_rejected() {
    for (source, message) in [
        ("trait A { type Out; fn get(&self) -> Self::Out; } impl A for i16 { type Out = bool; fn get(&self) -> i16 { 1 } }", "return type"),
        ("trait A { type Out; fn set(&self, x:Self::Out); } impl A for i16 { type Out = bool; fn set(&self, x:i16) {} }", "signature"),
        ("trait A { type Out; type Out; }", "duplicate associated type"),
        ("trait A {} impl A for i16 { type Extra = i16; }", "no associated type"),
        ("trait A { type Out; } impl A for i16 { type Out=i16; type Out=bool; }", "duplicate associated type"),
        ("trait A { type Out; } impl A for i16 { type Out=Missing; }", "unknown type"),
        ("trait A { type Out; } impl A for i16 { type Out=T; }", "unknown type"),
        ("trait A { type Out; } impl A for i16 { type Out=&i16; }", "borrowed reference"),
        ("trait A { type Out; } impl A for i16 { type Out=(i16, &i16); }", "borrowed reference"),
        ("trait A { type Out; } impl A for i16 { type Out=Vec<&i16>; }", "borrowed references"),
        ("trait A { type Out=i16; }", "defaults"),
        ("trait A { type Out<T>; }", "generic associated types"),
        ("impl i16 { type Out=i16; }", "trait impl"),
        ("trait A { type Out; } impl A for i16 { type Out:Printable=i16; }", "bounds belong"),
        ("trait A { type Out; } impl<T:A> A for Vec<T::Out> { type Out=i16; }", "impl type patterns"),
    ] { reject(source, message); }
}

#[test]
fn associated_aliases_detect_direct_indirect_and_cross_impl_cycles() {
    for source in [
        "trait A { type X; } impl A for i16 { type X=Self::X; }",
        "trait A { type X; type Y; } impl A for i16 { type X=Self::Y; type Y=Self::X; }",
        "trait A { type X; } impl A for i16 { type X=Vec<Self::X>; }",
        "trait A { type X; } impl A for i16 { type X=bool::X; } impl A for bool { type X=i16::X; }",
        "trait A { type X; } impl<T> A for Vec<T> { type X=Vec<Vec<T>>::X; }",
    ] {
        reject(source, "cycle");
    }
    check("trait A { type X; type Y; } impl A for i16 { type X=Self::Y; type Y=String; } fn main() { let x:i16::X = \"ok\"; }");
}

#[test]
fn qualified_projections_disambiguate_contracts_and_self_means_this_impl() {
    let source = r#"
        trait A { type Out; fn get(&self) -> Self::Out; }
        trait B { type Out; fn get(&self) -> Self::Out; }
        impl A for i16 { type Out = i16; fn get(&self) -> Self::Out { *self } }
        impl B for i16 { type Out = bool; fn get(&self) -> Self::Out { true } }
        fn first<T:A+B>(x:T) -> <T as A>::Out { A::get(&x) }
        fn second<T:A+B>(x:T) -> <T as B>::Out { B::get(&x) }
        fn main() -> i16 {
            let n:<i16 as A>::Out = first(42);
            let b:<i16 as B>::Out = second(0);
            if (b) { n } else { 1 }
        }
    "#;
    assert_eq!(rfl::run_native_program(source).unwrap().status, 42);
    reject(
        &source.replace("-> <T as A>::Out", "-> T::Out"),
        "ambiguous associated type",
    );
    reject(
        &source.replace("let n:<i16 as A>::Out", "let n:i16::Out"),
        "ambiguous associated type",
    );
    reject(
        "trait A { type Out; } fn f<T>(x:T) -> <T as A>::Out { x }",
        "does not implement",
    );
    reject(
        "trait A { type Out; } impl A for i16 { type Out=i16; } fn f(x:<i16 as Missing>::Out) {}",
        "unknown trait",
    );
}

#[test]
fn a_polling_future_can_return_its_associated_output() {
    let source = r#"
        type Poll<T> = Pending | Ready(T);
        trait Future { type Output: Printable; fn poll(&mut self) -> Poll<Self::Output>; }
        struct Number { value: i16 }
        impl Future for Number {
            type Output = String;
            fn poll(&mut self) -> Poll<Self::Output> {
                if (self.value == 0) { self.value = 1; Pending } else { Ready("ready") }
            }
        }
        fn step<F: Future>(future: &mut F) -> Option<F::Output> {
            match future.poll() { Pending => None, Ready(value) => Some(value) }
        }
        fn main() {
            let mut future = Number { value: 0 };
            println(step(&mut future));
            println(step(&mut future));
        }
    "#;
    let native = rfl::run_native_program(source).unwrap();
    assert_eq!(native.status, 0);
    assert_eq!(native.stdout, "None\nSome(ready)\n");
    let ripple = rfl::run_program(source).unwrap();
    assert_eq!(ripple.status, 0);
    assert_eq!(ripple.stdout, native.stdout);
    let ir = rcc_ir::emit_module(&rfl::compile_to_ir(source).unwrap());
    assert!(
        !ir.contains("$param$") && !ir.contains("$bound$"),
        "unresolved generic contract in IR"
    );
}

#[test]
fn generic_impls_and_managed_multicell_output_layouts() {
    let source = r#"
        trait Produce { type Output: Printable; fn produce(&self) -> Self::Output; }
        type Boxed<T> = Boxed(T);
        impl<T:Printable> Produce for Boxed<T> {
            type Output = T;
            fn produce(&self) -> Self::Output { match self { Boxed(value) => *value } }
        }
        fn get<P:Produce>(p:P) -> P::Output { p.produce() }
        fn main() {
            println(get(Boxed("hello")));
            println(get(Boxed([Some("nested"), None])));
            let n: <Boxed<i16> as Produce>::Output = get(Boxed(42));
            println(n);
        }
    "#;
    for result in [
        rfl::run_native_program(source).unwrap(),
        rfl::run_program(source).unwrap(),
    ] {
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "hello\n[Some(nested), None]\n42\n");
    }
}

#[test]
fn self_and_projection_types_are_usable_in_impl_bodies_and_type_arguments() {
    check(
        r#"
        trait A { type Out; fn get(&self) -> Self::Out; fn copy(&self) -> Self; }
        struct S { n:i16 }
        impl A for S {
            type Out = (String, Vec<i16>);
            fn get(&self) -> Self::Out { let x:Self::Out = ("hello", [self.n]); identity<Self::Out>(x) }
            fn copy(&self) -> Self { let x:Self = S { n:self.n }; x }
        }
        fn identity<T>(x:T) -> T { x }
        fn main() { let s=S{n:42}; let value:S::Out = identity<<S as A>::Out>(s.get()); let copy=s.copy(); }
    "#,
    );
}

#[test]
fn concrete_projections_work_in_fields_arrays_functions_and_constants() {
    check(
        r#"
        trait A { type N; }
        struct S { n:i16 }
        impl A for S { type N = usize; }
        const SIZE: S::N = 2;
        struct Data { values:[S::N; SIZE], other:Vec<S::N> }
        type Wrapped<T> = Wrap(T);
        fn value(x:S::N) -> S::N { x }
        fn main() {
            let f:fn(S::N)->S::N = value;
            let fixed:[S::N; SIZE] = [1, 2];
            let data = Data { values:fixed, other:[f(42)] };
            let wrapped:Wrapped<S::N> = Wrap(data.values[0]);
        }
    "#,
    );
}

#[test]
fn projection_arguments_use_the_base_type_without_inverting_the_projection() {
    check(
        r#"
        trait A { type Out; }
        struct S { n:i16 }
        impl A for S { type Out=u32; }
        fn accept<T:A>(out:T::Out, values:Vec<T::Out>, value:T) -> T::Out { out }
        fn main() { let value:u32=accept(42, [], S{n:0}); }
    "#,
    );
    reject(
        "trait A { type Out; } fn bad<T:A>(out:T::Out) {} fn main() { bad(1); }",
        "cannot infer",
    );
    reject("trait A { type Out; } struct S{n:i16} impl A for S { type Out=bool; } fn accept<T:A>(out:T::Out, x:T) {} fn main() { accept(42, S{n:0}); }", "expected");
    reject(
        r#"
        trait A { type Out; }
        struct S { n:i16 }
        impl A for S { type Out=i16; }
        fn change(value:&mut i16) -> S { *value=2; S{n:0} }
        fn accept<T:A>(value:&T::Out, owner:T) {}
        fn main() { let mut value=1; accept(&value, change(&mut value)); }
    "#,
        "already borrowed",
    );
}

#[test]
fn nested_projections_follow_declared_bounds() {
    check(
        r#"
        trait Item { type Value: Printable; fn value(&self) -> Self::Value; }
        trait Source { type Output: Item; fn output(&self) -> Self::Output; }
        impl Item for i16 { type Value=String; fn value(&self) -> String { "nested" } }
        impl Source for bool { type Output=i16; fn output(&self) -> i16 { 42 } }
        fn nested<T:Source>(x:T) -> T::Output::Value { x.output().value() }
        fn main() { println(nested(true)); }
    "#,
    );
}

#[test]
fn associated_struct_output_preserves_wide_layout_and_ownership() {
    let source = r#"
        trait Source { type Output; fn output(&self) -> Self::Output; }
        struct Data { text:String, values:Vec<i16>, number:i64 }
        impl Source for bool {
            type Output=Data;
            fn output(&self) -> Self::Output { Data { text:"owned", values:[20, 22], number:1234567890123 } }
        }
        fn get<T:Source>(x:T) -> T::Output { x.output() }
        fn main() -> i16 {
            let first:bool::Output=get(true);
            let mut copy=first;
            copy.values.push(7);
            println(first.text);
            println(first.values);
            println(copy.values);
            println(first.number);
            first.values[0]+first.values[1]
        }
    "#;
    for result in [
        rfl::run_native_program(source).unwrap(),
        rfl::run_program(source).unwrap(),
    ] {
        assert_eq!(result.status, 42);
        assert_eq!(
            result.stdout,
            "owned\n[20, 22]\n[20, 22, 7]\n1234567890123\n"
        );
    }
}

#[test]
fn associated_types_cross_module_aliases_and_keep_lexical_visibility() {
    struct Directory(std::path::PathBuf);
    impl Drop for Directory {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let dir = Directory(
        std::env::temp_dir().join(format!("rfl-associated-{}-{stamp}", std::process::id())),
    );
    std::fs::create_dir_all(&dir.0).unwrap();
    std::fs::write(
        dir.0.join("source.rfl"),
        r#"
        export trait Source { type Output; fn output(&self) -> Self::Output; }
        export struct Value { n:i16 }
        impl Source for Value { type Output=i16; fn output(&self) -> i16 { self.n } }
    "#,
    )
    .unwrap();
    let path = dir.0.join("main.rfl");
    let source = r#"
        import { Source as Produce, Value as Number } from "./source";
        fn output<T:Produce>(x:T) -> T::Output { x.output() }
        fn main() -> i16 { let x:<Number as Produce>::Output=output(Number{n:42}); x }
    "#;
    std::fs::write(&path, source).unwrap();
    assert_eq!(rfl::run_native_file(&path, false).unwrap().status, 42);
    std::fs::write(
        &path,
        "import { Value } from \"./source\"; fn main() { let x:Value::Output=42; }",
    )
    .unwrap();
    assert!(rfl::check_file(&path)
        .unwrap_err()
        .message
        .contains("associated type"));
}

#[test]
fn self_and_unsupported_bound_forms_have_explicit_diagnostics() {
    for source in [
        "struct Self { n:i16 }",
        "trait Self {}",
        "type Self = Empty;",
        "fn f<Self>(x:Self) {}",
        "trait A { type Self; }",
    ] {
        reject(source, "reserved");
    }
    reject(
        "trait A { type Out; } fn f<T:A<Out=i16>>(x:T) {}",
        "equality constraints",
    );
    check("fn main() { let n=8 << 2; let m=n >> 1; println(m); }");
}
