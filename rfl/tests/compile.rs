fn ir(source: &str) -> String {
    rfl::compile_to_ir_text(source).unwrap_or_else(|err| panic!("{err}"))
}

fn asm(source: &str) -> String {
    rfl::compile_to_asm(source).unwrap_or_else(|err| panic!("{err}"))
}

#[test]
fn main_returns_a_constant() {
    let text = ir("fn main() -> i16 { 42 }\n");
    assert!(text.contains("function i16 @main"), "{text}");
    assert!(text.contains("function i16* @rfl_fn_main"), "{text}");
    assert!(text.contains("42"), "{text}");
    assert!(!text.contains("rfl_frame_alloc"), "{text}");
    let assembly = asm("fn main() -> i16 { 42 }\n");
    assert!(assembly.contains("main"), "{assembly}");
}

#[test]
fn arithmetic_let_and_if() {
    let source = r#"
        fn main() -> i16 {
            let x = 20;
            let y = 22;
            if (x < y) { x + y } else { 0 }
        }
    "#;
    let assembly = asm(source);
    assert!(assembly.contains("main"), "{assembly}");
}

#[test]
fn call_uses_a_heap_frame() {
    let source = r#"
        fn add(a: i16, b: i16) -> i16 { a + b }
        fn main() -> i16 { add(20, 22) }
    "#;
    let text = ir(source);
    assert!(text.contains("rfl_frame_alloc"), "{text}");
    assert!(text.contains("@rfl_fn_add"), "{text}");
    let assembly = asm(source);
    assert!(assembly.contains("rfl_frame_alloc"), "{assembly}");
}

#[test]
fn immutable_binding_is_rejected() {
    let source = "fn main() -> i16 {\n  let x = 1;\n  x = 2;\n  x\n}\n";
    let err = rfl::compile_to_ir(source).unwrap_err();
    assert!(err.to_string().contains("not mutable"), "{err}");
}

#[test]
fn mutable_binding_round_trips_through_asm() {
    let source = "fn main() -> i16 {\n  let mut x = 1;\n  x = x + 41;\n  x\n}\n";
    asm(source);
}

#[test]
fn factorial_loop_lowers() {
    let source = r#"
        fn main() -> i16 {
            let mut result = 1;
            let mut i = 2;
            while (i <= 5) {
                result *= i;
                i += 1;
            }
            result
        }
    "#;
    asm(source);
}

#[test]
fn calls_run_on_the_vm() {
    let source = r#"
        fn add(a: i16, b: i16) -> i16 { a + b }
        fn main() -> i16 { add(20, 22) }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);

    let source = r#"
        fn sum(n: i16) -> i16 {
            if (n <= 0) { 0 } else { n + sum(n - 1) }
        }
        fn main() -> i16 { sum(10) }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 55, "{}", result.stdout);

    let source = r#"
        fn burn(n: i16) -> i16 {
            if (n <= 0) { 0 } else { burn(n - 1) }
        }
        fn main() -> i16 {
            let mut i = 0;
            while (i < 40) {
                burn(6);
                i += 1;
            }
            i
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 40, "{}", result.stdout);
}

#[test]
fn programs_run_on_the_vm() {
    let forty = rfl::run_program("fn main() -> i16 { 42 }\n").unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(forty.status, 42, "{}", forty.stdout);
    let source = r#"
        fn main() -> i16 {
            let mut result = 1;
            let mut i = 2;
            while (i <= 5) {
                result *= i;
                i += 1;
            }
            result
        }
    "#;
    let fact = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(fact.status, 120, "stdout {} ", fact.stdout);
}

#[test]
fn print_logs_values() {
    let source = r#"
        fn main() -> i16 {
            print(-42);
            putc(' ');
            println(7);
            println("ok");
            println(true);
            println(false);
            let n: u16 = 65535;
            println(n);
            let w: i32 = -100000;
            println(w);
            putc('Z');
            9
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 9, "{}", result.stdout);
    assert_eq!(result.stdout, "-42 7\nok\ntrue\nfalse\n65535\n-100000\nZ");
}

#[test]
fn string_values_are_rejected() {
    let err = rfl::compile_to_ir("fn main() -> i16 { \"hi\" }\n").unwrap_err();
    assert!(err.to_string().contains("string values"), "{err}");
}

#[test]
fn map_runs_on_the_vm() {
    let source = r#"
        type List<T> = | Nil | Cons(T, List<T>);
        fn map<T, U>(xs: List<T>, f: fn(T) -> U) -> List<U> {
            match xs {
                Nil => Nil,
                Cons(x, rest) => Cons(f(x), map(rest, f)),
            }
        }
        fn double(x: i16) -> i16 {
            x * 2
        }
        fn sum(xs: List<i16>) -> i16 {
            match xs {
                Nil => 0,
                Cons(x, rest) => x + sum(rest),
            }
        }
        fn main() -> i16 {
            let inc = fn(x: i16) => x + 1;
            sum(map(Cons(1, Cons(2, Cons(3, Nil))), double)) + inc(0)
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 13, "{}", result.stdout);
}

#[test]
fn closure_captures_by_value() {
    let source = r#"
        fn main() -> i16 {
            let mut factor = 2;
            let multiply = fn(x) => x * factor;
            factor = 3;
            multiply(21)
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn method_calls_run_on_the_vm() {
    let source = r#"
        type List<T> = | Nil | Cons(T, List<T>);
        fn map<T, U>(xs: List<T>, f: fn(T) -> U) -> List<U> {
            match xs {
                Nil => Nil,
                Cons(x, rest) => Cons(f(x), map(rest, f)),
            }
        }
        fn double(x: i16) -> i16 { x * 2 }
        fn sum(xs: List<i16>) -> i16 {
            match xs {
                Nil => 0,
                Cons(x, rest) => x + sum(rest),
            }
        }
        fn squared(x: i16) -> i16 { x * x }
        fn main() -> i16 {
            let numbers = Cons(1, Cons(2, Cons(3, Nil)));
            numbers.map(double).sum() + 10.squared()
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 112, "{}", result.stdout);
}

#[test]
fn generic_list_runs_on_the_vm() {
    let source = r#"
        type List<T> = | Nil | Cons(T, List<T>);
        fn sum(xs: List<i16>) -> i16 {
            match xs {
                Nil => 0,
                Cons(x, rest) => x + sum(rest),
            }
        }
        fn len<T>(xs: List<T>) -> i16 {
            match xs {
                Nil => 0,
                Cons(_, rest) => 1 + len(rest),
            }
        }
        fn singleton<T>(x: T) -> List<T> {
            Cons(x, Nil)
        }
        fn identity<T>(x: T) -> T { x }
        type Pair<A, B> = | Pair(A, B);
        fn first(p: Pair<i16, bool>) -> i16 {
            match p { Pair(a, _) => a }
        }
        fn main() -> i16 {
            let wide: i32 = identity(100000);
            let tagged: i32 = identity<i32>(7);
            let nested: List<List<i16>> = Cons(Nil, Nil);
            if (wide == 100000 && tagged == 7 && len(Cons(true, Cons(false, Nil))) == 2 && first(Pair(1, true)) == 1) {
                match nested {
                    Nil => 0,
                    Cons(_, _) => sum(singleton(identity(42))),
                }
            } else {
                0
            }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn nil_without_a_type_is_rejected() {
    let source = "type List<T> = | Nil | Cons(T, List<T>);\nfn main() -> i16 { let x = Nil; 1 }\n";
    let err = rfl::compile_to_ir(source).unwrap_err();
    assert!(err.to_string().contains("cannot infer"), "{err}");
}

#[test]
fn lists_run_on_the_vm() {
    let source = r#"
        type List = | Nil | Cons(i16, List);
        fn sum(xs: List) -> i16 {
            match xs {
                Nil => 0,
                Cons(x, rest) => x + sum(rest),
            }
        }
        fn main() -> i16 {
            sum(Cons(20, Cons(22, Nil)))
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn non_exhaustive_match_is_rejected() {
    let source = "type Option = | None | Some(i16);\nfn main() -> i16 { match None { None => 1 } }\n";
    let err = rfl::compile_to_ir(source).unwrap_err();
    assert!(err.to_string().contains("non-exhaustive"), "{err}");
}

#[test]
fn break_outside_a_loop_is_rejected() {
    let err = rfl::compile_to_ir("fn main() { break; }\n").unwrap_err();
    assert!(err.to_string().contains("outside a loop"), "{err}");
}

#[test]
fn i8_addition_truncates() {
    let source = "fn add(a: i8, b: i8) -> i8 { a + b }\nfn main() -> i16 { 0 }\n";
    let text = ir(source);
    assert!(text.contains("trunc"), "{text}");
}
