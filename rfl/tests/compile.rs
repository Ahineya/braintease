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
fn vectors_keep_value_semantics() {
    let source = r#"
        fn main() -> i16 {
            let a = [1, 2, 3];
            let mut b = a;
            b.push(4);
            let mut c = [10];
            c.push(20);
            let empty: Vec<i16> = [];
            if (a.len() == 3 && b.len() == 4 && a[0] == 1 && a[2] == 3 && b[3] == 4
                && c.len() == 2 && c[0] == 10 && c[1] == 20 && empty.len() == 0) {
                42
            } else {
                0
            }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn vector_pop_and_get_return_option() {
    let source = r#"
        fn main() -> i16 {
            let a = [10, 20, 30];
            let mut b = a;
            let top = b.pop();
            let mid = a.get(1);
            let missing = a.get(5);
            let mut empty: Vec<i16> = [];
            match top {
                Some(x) => match mid {
                    Some(y) => match missing {
                        None => match empty.pop() {
                            None => if (a.len() == 3 && b.len() == 2 && x == 30 && y == 20 && b[1] == 20) { 42 } else { 0 },
                            Some(_) => 0,
                        },
                        Some(_) => 0,
                    },
                    None => 0,
                },
                None => 0,
            }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn vector_updates_keep_other_values() {
    let source = r#"
        fn main() -> i16 {
            let a = [1, 2, 3];
            let mut b = a;
            b[1] = 8;
            let d = [5];
            let mut e = d;
            e.clear();
            if (a[1] == 2 && b[1] == 8 && b.len() == 3 && d[0] == 5 && e.is_empty() && !a.is_empty()) {
                42
            } else {
                0
            }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn vector_index_assign_needs_mut() {
    let err = rfl::compile_to_ir("fn main() { let a = [1]; a[0] = 2; }\n").unwrap_err();
    assert!(err.to_string().contains("not mutable"), "{err}");
}

#[test]
fn vector_index_assign_is_bounds_checked() {
    let source = "fn main() -> i16 { let mut a = [7]; a[1] = 3; 0 }\n";
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 1, "{}", result.stdout);
    assert!(result.stdout.contains("fail"), "{}", result.stdout);
}

#[test]
fn empty_vector_needs_a_type() {
    let err = rfl::compile_to_ir("fn main() { let a = []; }\n").unwrap_err();
    assert!(err.to_string().contains("element type"), "{err}");
}

#[test]
fn vector_index_is_bounds_checked() {
    let result = rfl::run_program("fn main() -> i16 { let a = [7]; a[1] }\n").unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 1, "{}", result.stdout);
    assert!(result.stdout.contains("fail"), "{}", result.stdout);
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
    let source = "type Choice = | No | Yes(i16);\nfn main() -> i16 { match No { No => 1 } }\n";
    let err = rfl::compile_to_ir(source).unwrap_err();
    assert!(err.to_string().contains("non-exhaustive"), "{err}");
}

#[test]
fn break_outside_a_loop_is_rejected() {
    let err = rfl::compile_to_ir("fn main() { break; }\n").unwrap_err();
    assert!(err.to_string().contains("outside a loop"), "{err}");
}

#[test]
fn references_update_the_borrowed_vector() {
    let source = r#"
        fn sum(xs: &Vec<i16>) -> i16 { xs[0] + xs[1] }
        fn append(xs: &mut Vec<i16>, value: i16) { xs.push(value); }
        fn main() -> i16 {
            let a = [10, 20];
            let mut b = a;
            append(&mut b, 12);
            {
                let borrowed = &mut b;
                borrowed.push(0);
            }
            if (sum(&a) == 30 && b.len() == 4 && b[2] == 12 && a.len() == 2) { 42 } else { 0 }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn shared_borrow_blocks_mutation() {
    let source = "fn main() { let mut numbers = [1]; let view = &numbers; numbers.push(2); view.len(); }\n";
    let err = rfl::compile_to_ir(source).unwrap_err();
    assert!(err.to_string().contains("already borrowed"), "{err}");
}

#[test]
fn mutable_reference_is_required_to_push() {
    let source = "fn main() { let numbers = [1]; let view = &numbers; view.push(2); }\n";
    let err = rfl::compile_to_ir(source).unwrap_err();
    assert!(err.to_string().contains("mutable reference"), "{err}");
}

#[test]
fn mutable_borrow_is_exclusive() {
    let source = "fn main() { let mut numbers = [1]; let a = &mut numbers; let b = &mut numbers; a.len(); b.len(); }\n";
    let err = rfl::compile_to_ir(source).unwrap_err();
    assert!(err.to_string().contains("already borrowed"), "{err}");
}

#[test]
fn reference_cannot_escape_a_block() {
    let source = "fn main() { let numbers = [1]; let view = { &numbers }; view.len(); }\n";
    let err = rfl::compile_to_ir(source).unwrap_err();
    assert!(err.to_string().contains("cannot escape"), "{err}");
}

#[test]
fn inherent_methods_chain_and_beat_free_functions() {
    let source = r#"
        impl<T> Option<T> {
            fn unwrap_or(self, fallback: T) -> T {
                match self { Some(value) => value, None => fallback }
            }
            fn map<U>(self, f: fn(T) -> U) -> Option<U> {
                match self { Some(value) => Some(f(value)), None => None }
            }
        }
        impl i16 {
            fn length(self) -> i16 { 7 }
            fn answer() -> i16 { 42 }
        }
        fn length(x: i16) -> i16 { 100 }
        impl<T> Vec<T> {
            fn grow(&mut self, value: T) { self.push(value); }
        }
        fn main() -> i16 {
            let mut numbers = [1];
            numbers.grow(2);
            let mapped = Some(21).map(fn(x: i16) => x * 2).unwrap_or(0);
            if (mapped == 42 && numbers.len() == 2 && numbers[1] == 2 && 1.length() == 7 && length(1) == 100 && i16::answer() == 42) { 42 } else { 0 }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn inherent_method_does_not_fall_through() {
    let source = "impl i16 { fn length(self, extra: i16) -> i16 { extra } }\nfn length(x: i16) -> i16 { 100 }\nfn main() -> i16 { 1.length() }\n";
    let err = rfl::compile_to_ir(source).unwrap_err();
    assert!(err.to_string().contains("expects 1"), "{err}");
}

#[test]
fn question_propagates_option_and_result() {
    let source = r#"
        fn first(xs: Vec<i16>) -> Option<i16> { Some(xs.get(0)?) }
        fn div(a: i16, b: i16) -> Result<i16, i16> {
            if (b == 0) { Err(0) } else { Ok(a / b) }
        }
        fn run(a: i16, b: i16) -> Result<i16, i16> { Ok(div(a, b)? + 1) }
        fn main() -> i16 {
            let got = first([7]);
            match got {
                Some(value) => match run(20, 2) {
                    Ok(total) => match run(1, 0) {
                        Err(_) => if (value == 7 && total == 11) { 42 } else { 0 },
                        Ok(_) => 0,
                    },
                    Err(_) => 0,
                },
                None => 0,
            }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn question_needs_option_or_result() {
    let err = rfl::compile_to_ir("fn main() -> i16 { let x = 1; x? }\n").unwrap_err();
    assert!(err.to_string().contains("Option or Result"), "{err}");
}

#[test]
fn destructure_and_guards_and_constants() {
    let source = r#"
        type Point = | Point(i16, i16);
        type Pair = | Pair(Point, i16);
        const WIDTH: usize = 20;
        const HEIGHT: i16 = 22;
        const AREA: usize = WIDTH * 22;
        const FLAG: bool = WIDTH < 100 && HEIGHT == 22;
        const STEP: i16 = (1 + 2) * 3;
        fn classify(n: i16) -> i16 {
            let value = if (n > 10) { Some(n) } else { if (n > 0) { Some(n) } else { None } };
            match value {
                Some(x) if (x > 10) => 1,
                Some(x) if (x > 0) => 2,
                Some(_) => 3,
                None => 4,
            }
        }
        fn main() -> i16 {
            let Point(x, y) = Point(20, 22);
            let mut Pair(Point(a, b), c) = Pair(Point(1, 1), 1);
            a = 40;
            if (x + y == 42 && a + b + c == 42 && classify(21) == 1 && classify(3) == 2 && classify(0) == 4 && FLAG && AREA == 440 && STEP == 9) { 42 } else { 0 }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn refutable_let_is_rejected() {
    let err = rfl::compile_to_ir("fn main() -> i16 {\n  let Some(x) = Some(1);\n  x\n}\n").unwrap_err();
    assert!(err.to_string().contains("irrefutable"), "{err}");
}

#[test]
fn guarded_arm_does_not_cover() {
    let source = "fn main() -> i16 {\n  match Some(1) { Some(x) if (x > 0) => x, None => 0 }\n}\n";
    let err = rfl::compile_to_ir(source).unwrap_err();
    assert!(err.to_string().contains("non-exhaustive"), "{err}");
}

#[test]
fn constant_must_be_known() {
    let err = rfl::compile_to_ir("const B: i16 = A;\nconst A: i16 = 1;\nfn main() -> i16 { B }\n").unwrap_err();
    assert!(err.to_string().contains("unknown constant"), "{err}");
}

#[test]
fn slices_and_fixed_arrays() {
    let source = r#"
        const N: usize = 4;
        fn middle() -> Slice<i16> {
            let numbers = [1, 2, 3, 4];
            numbers[1..3]
        }
        fn main() -> i16 {
            let mut numbers = [1, 2, 3, 4];
            let slice = numbers[1..=2];
            numbers[1] = 99;
            let mut pixels: [i16; N] = [10, 20, 30, 40];
            let dynamic = pixels.to_vec();
            pixels[1] = 7;
            let tail = dynamic[2..4];
            let missed = pixels.get(4);
            let got = slice.get(0);
            match missed {
                None => match got {
                    Some(value) => if (numbers[1] == 99 && slice[0] == 2 && slice[1] == 3 && slice.len() == 2 && pixels[1] == 7 && pixels.len() == 4 && dynamic[1] == 20 && tail[0] == 30 && tail.len() == 2 && middle()[0] == 2 && value == 2 && numbers[2..2].is_empty()) { 42 } else { 0 },
                    None => 0,
                },
                Some(_) => 0,
            }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn slice_is_read_only() {
    let err = rfl::compile_to_ir("fn main() -> i16 {\n  let numbers = [1, 2];\n  let slice = numbers[0..1];\n  slice[0] = 5;\n  0\n}\n").unwrap_err();
    assert!(err.to_string().contains("read-only"), "{err}");
}

#[test]
fn managed_values_are_reclaimed() {
    let source = r#"
        type List = | Nil | Cons(i16, List);
        fn churn() -> i16 {
            let mut n = 0;
            while (n < 30) {
                let xs = Cons(n, Cons(1, Nil));
                match xs {
                    Cons(h, _) => { n = h + 1; },
                    Nil => { n = 100; },
                }
            }
            n
        }
        fn alias() -> i16 {
            let a = Cons(20, Cons(22, Nil));
            let b = a;
            match a {
                Cons(x, Cons(y, Nil)) => match b {
                    Cons(p, _) => if (x + y == 42 && p == 20) { 1 } else { 0 },
                    Nil => 0,
                },
                _ => 0,
            }
        }
        fn hold(n: i16) -> fn() -> i16 {
            let xs = Cons(n, Nil);
            fn() => match xs { Cons(h, _) => h, Nil => 0 }
        }
        fn main() -> i16 {
            let f = hold(40);
            if (churn() == 30 && alias() == 1 && f() == 40) { 42 } else { 0 }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn fixed_array_length_is_part_of_the_type() {
    let err = rfl::compile_to_ir("fn main() -> i16 {\n  let pixels: [i16; 2] = [1];\n  pixels[0]\n}\n").unwrap_err();
    assert!(err.to_string().contains("expected 2 elements"), "{err}");
}

#[test]
fn i8_addition_truncates() {
    let source = "fn add(a: i8, b: i8) -> i8 { a + b }\nfn main() -> i16 { 0 }\n";
    let text = ir(source);
    assert!(text.contains("trunc"), "{text}");
}

#[test]
fn omitted_parameter_types_are_inferred() {
    let source = r#"
        fn add(a, b) { a + b }
        fn identity(x) { x }
        fn main() -> i16 {
            if (identity(true)) { add(identity(20), 22) } else { 0 }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn omitted_map_generalizes() {
    let source = r#"
        type List<T> = | Nil | Cons(T, List<T>);
        fn map(f, xs) {
            match xs {
                Nil => Nil,
                Cons(x, rest) => Cons(f(x), map(f, rest)),
            }
        }
        fn sum(xs: List<i16>) -> i16 {
            match xs {
                Nil => 0,
                Cons(x, rest) => x + sum(rest),
            }
        }
        fn main() -> i16 {
            let numbers = Cons(10, Cons(11, Nil));
            sum(map(fn(x) => x * 2, numbers))
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn structs_and_tuples_are_values() {
    let source = r#"
        struct Point { x: i16, y: i16 }
        fn length_squared(Point { x, y }: Point) -> i16 { x * x + y * y }
        fn main() -> i16 {
            let mut p = Point { x: 20, y: 20 };
            p.y = 22;
            let Point { x, y } = p;
            let (a, b) = (x, y);
            if (length_squared(Point { x: 3, y: 4 }) == 25 && a + b == 42) { 42 } else { 0 }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn literal_patterns_match_values() {
    let source = r#"
        fn classify(n: i16) -> i16 {
            match n {
                0 => 1,
                _ => 2,
            }
        }
        fn flag(v: bool) -> i16 {
            match v {
                true => 1,
                false => 0,
            }
        }
        fn main() -> i16 {
            let tagged = match Some(0) {
                Some(0) => 1,
                Some(_) => 2,
                None => 3,
            };
            if (tagged == 1 && classify(0) == 1 && flag(true) == 1) { 42 } else { 0 }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn integer_match_must_be_exhaustive() {
    let err = rfl::compile_to_ir("fn main() -> i16 { match 1 { 0 => 1 } }\n").unwrap_err();
    assert!(err.to_string().contains("non-exhaustive"), "{err}");
}

#[test]
fn ranges_are_only_legal_in_for() {
    let err = rfl::compile_to_ir("fn main() -> i16 { 0..10 }\n").unwrap_err();
    assert!(err.to_string().contains("range"), "{err}");
}

#[test]
fn for_walks_ranges_vectors_and_lists() {
    let source = r#"
        type List<T> = | Nil | Cons(T, List<T>);
        fn range_sum() -> i16 {
            let mut n = 0;
            for (i in 0..10) { n = n + i; }
            n
        }
        fn inclusive_sum() -> i16 {
            let mut n = 0;
            for (i in 0..=10) { n = n + i; }
            n
        }
        fn skipped() -> i16 {
            let mut n = 0;
            for (i in 0..5) {
                if (i == 2) { continue; }
                n = n + i;
            }
            n
        }
        fn vec_sum() -> i16 {
            let xs = [10, 32];
            let mut n = 0;
            for (x in xs) { n = n + x; }
            n
        }
        fn list_sum() -> i16 {
            let xs = Cons(20, Cons(22, Nil));
            let mut n = 0;
            for (x in xs) { n = n + x; }
            n
        }
        fn main() -> i16 {
            if (range_sum() == 45 && inclusive_sum() == 55 && skipped() == 8 && vec_sum() == 42 && list_sum() == 42) {
                42
            } else {
                0
            }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn tail_calls_reuse_the_frame() {
    let source = r#"
        fn go(n: i16, acc: i16) -> i16 {
            if (n <= 0) { acc } else { go(n - 1, acc + n) }
        }
        fn deep(n: i16, acc: i16) -> i16 {
            if (n <= 0) { acc } else { deep(n - 1, acc + 1) }
        }
        fn main() -> i16 {
            if (go(8, 6) == 42 && deep(200, 0) == 200) { 42 } else { 0 }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn products_keep_managed_fields() {
    let source = r#"
        struct Hold { items: Vec<i16> }
        type Wrap = | Wrap(Hold);
        fn make() -> Hold { Hold { items: [20, 22] } }
        fn make_pair() -> (Vec<i16>, i16) { ([20, 22], 0) }
        fn capture() -> fn() -> i16 {
            let held = Hold { items: [20, 22] };
            fn() => held.items[0] + held.items[1]
        }
        fn unwrap(value: Wrap) -> Hold {
            match value {
                Wrap(held) => held,
            }
        }
        fn spoil() { let junk = [1, 2]; if (junk[0] == 1) { } }
        fn main() -> i16 {
            let held = make();
            spoil();
            let from_fn = held.items[0] + held.items[1];
            let pair = make_pair();
            spoil();
            let (xs, _) = pair;
            let from_pair = xs[0] + xs[1];
            let f = capture();
            spoil();
            let from_closure = f();
            let wrapped = unwrap(Wrap(Hold { items: [10, 32] }));
            spoil();
            let from_adt = wrapped.items[0] + wrapped.items[1];
            if (from_fn == 42 && from_pair == 42 && from_closure == 42 && from_adt == 42) { 42 } else { 0 }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}
