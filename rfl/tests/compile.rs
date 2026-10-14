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
fn strings_compare_index_and_print() {
    let source = r#"
        struct Hold { text: String }
        fn make() -> Hold { Hold { text: "Hi" } }
        fn spoil() { let _gone = "NO"; }
        fn main() -> i16 {
            let h = make();
            spoil();
            let s = h.text;
            let t = s;
            println(s);
            putc('!');
            let code: i32 = 'A' as i32;
            if (t == "Hi" && s != "" && s.len() == 2 && !s.is_empty() && s[0] == 'H' && 'A' < 'B' && code == 65) {
                42
            } else {
                0
            }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.stdout, "Hi\n!", "status {}", result.status);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn string_is_not_a_c_type() {
    let err = rfl::compile_to_ir("extern \"C\" { fn f(s: String); }\nfn main() -> i16 { 0 }\n").unwrap_err();
    assert!(err.to_string().contains("C signature"), "{err}");
}

#[test]
fn surrogate_cast_fails() {
    let source = "fn main() -> i16 {\n  let n: i32 = 55296;\n  let _c = n as char;\n  42\n}\n";
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert!(result.stdout.contains("fail"), "{}", result.stdout);
    assert_eq!(result.status, 1, "{}", result.stdout);
}

#[test]
fn char_is_not_an_integer() {
    let err = rfl::compile_to_ir("fn main() -> i16 { let n: i32 = 65; let _c: char = n; 0 }\n").unwrap_err();
    assert!(err.to_string().contains("char"), "{err}");
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
fn for_range_uses_the_collection_length() {
    let source = r#"
        fn main() -> i16 {
            let program = "ab";
            let mut sum: i16 = 0;
            for (i in 0..program.len()) {
                println(program[i]);
                sum = sum + (program[i] as i16);
            }
            if (sum == 195) { 42 } else { 0 }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.stdout, "a\nb\n", "status {}", result.status);
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

#[test]
fn floats_use_softfloat() {
    let source = r#"
        const STEP: f64 = 0.5;
        fn add(a: f64, b: f64) -> f64 { a + b }
        fn mul32(a: f32, b: f32) -> f32 { a * b }
        fn neg(a: f64) -> f64 { -a }
        fn div(a: f64, b: f64) -> f64 { a / b }
        fn cut(a: f64) -> i16 { a as i16 }
        fn widen(a: i16) -> f64 { a as f64 }
        fn shift(x) { x + 0.5 }
        fn main() -> i16 {
            let wide = add(20.5, 21.5);
            let narrow = mul32(1.5, 2.0);
            let down = neg(1.5);
            let ratio = div(5.0, 2.0);
            let from_int = widen(20);
            let trunc = cut(3.9);
            let widened = mul32(1.5, 1.0) as f64;
            let inf = div(1.0, 0.0);
            let nan = div(0.0, 0.0);
            let xs = [10.0, 32.0];
            let stepped = add(1.0, STEP);
            let shifted = shift(19.5);
            if (wide != 42.0) { 1 }
            else if (narrow != 3.0) { 2 }
            else if (!(down < 0.0 && down == -1.5)) { 3 }
            else if (ratio != 2.5) { 4 }
            else if (from_int != 20.0) { 5 }
            else if (trunc != 3) { 6 }
            else if (widened != 1.5) { 7 }
            else if (!(inf > 1.0)) { 8 }
            else if (nan == nan) { 9 }
            else if (xs[0] + xs[1] != 42.0) { 10 }
            else if (stepped != 1.5) { 11 }
            else if (shifted != 20.0) { 12 }
            else { 42 }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn float_and_integer_do_not_mix() {
    let err = rfl::compile_to_ir("fn main() -> i16 { if (1.0 + 1 == 2.0) { 1 } else { 0 } }\n").unwrap_err();
    assert!(err.to_string().contains("integer"), "{err}");
}

#[test]
fn float_remainder_is_rejected() {
    let err = rfl::compile_to_ir("fn main() -> i16 { if (1.0 % 2.0 == 0.0) { 1 } else { 0 } }\n").unwrap_err();
    assert!(err.to_string().contains("integers"), "{err}");
}

#[test]
fn c_is_a_name_until_a_quote() {
    let source = r#"
        fn c(c: i16) -> i16 { c }
        fn main() -> i16 { c(42) }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn export_reenters_through_c() {
    let source = r#"
        export extern "C" fn add(a: c_int, b: c_int) -> c_int { a + b }
        fn main() -> i16 { add(20, 22) }
    "#;
    let text = ir(source);
    assert!(text.contains("function i16 @\"add\""), "{text}");
    assert!(text.contains("function i16* @rfl_fn_add"), "{text}");
    assert!(text.contains("call i16 global @\"add\""), "{text}");
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn c_string_calls_puts() {
    let source = r#"
        extern "C" {
            fn puts(s: *const c_char) -> c_int;
        }
        fn main() -> i16 {
            puts(c"Y");
            42
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.stdout, "Y\n", "status {}", result.status);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn c_signature_rejects_a_vector() {
    let err = rfl::compile_to_ir(
        "extern \"C\" { fn bad(v: Vec<i16>) -> c_int; }\nfn main() -> i16 { 0 }\n",
    )
    .unwrap_err();
    assert!(err.to_string().contains("C signature"), "{err}");
}

struct Tmp(std::path::PathBuf);

impl Drop for Tmp {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn tmp_main(files: &[(&str, &str)]) -> (Tmp, std::path::PathBuf) {
    static SEQ: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let n = SEQ.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let dir = std::env::temp_dir().join(format!("rfl-mod-{}-{n}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    for (name, body) in files {
        std::fs::write(dir.join(name), body).unwrap();
    }
    let main = dir.join("main.rfl");
    (Tmp(dir), main)
}

#[test]
fn import_needs_a_source_file() {
    let err = rfl::compile_to_ir("import { sum } from \"./math\";\nfn main() -> i16 { 0 }\n").unwrap_err();
    assert!(err.to_string().contains("source file"), "{err}");
}

#[test]
fn standard_library_is_not_implemented() {
    let (_tmp, main) = tmp_main(&[("main.rfl", "import { foo } from \"std/foo\";\nfn main() -> i16 { 0 }\n")]);
    let err = rfl::compile_file(&main).unwrap_err();
    assert!(err.to_string().contains("standard library"), "{err}");
}

#[test]
fn import_renames_a_function() {
    let (_tmp, main) = tmp_main(&[
        ("math.rfl", "export fn sum(a: i16, b: i16) -> i16 { a + b }\n"),
        ("main.rfl", "import { sum as add } from \"./math\";\nfn main() -> i16 { add(20, 22) }\n"),
    ]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn private_import_is_rejected() {
    let (_tmp, main) = tmp_main(&[
        ("lib.rfl", "fn hidden() -> i16 { 1 }\n"),
        ("main.rfl", "import { hidden } from \"./lib\";\nfn main() -> i16 { hidden() }\n"),
    ]);
    let err = rfl::compile_file(&main).unwrap_err();
    assert!(err.to_string().contains("not exported"), "{err}");
}

#[test]
fn import_cycle_is_rejected() {
    let (tmp, _main) = tmp_main(&[
        ("a.rfl", "import { b } from \"./b\";\nexport fn a() -> i16 { b() }\nfn main() -> i16 { a() }\n"),
        ("b.rfl", "import { a } from \"./a\";\nexport fn b() -> i16 { a() }\n"),
    ]);
    let err = rfl::compile_file(&tmp.0.join("a.rfl")).unwrap_err();
    assert!(err.to_string().contains("cycle"), "{err}");
}

#[test]
fn export_type_brings_constructors() {
    let (_tmp, main) = tmp_main(&[
        ("choice.rfl", "export type Choice = No | Yes(i16);\n"),
        (
            "main.rfl",
            "import { Choice } from \"./choice\";\nfn main() -> i16 {\n    let c = Yes(42);\n    match (c) { Yes(n) => n, No => 0 }\n}\n",
        ),
    ]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn import_c_function() {
    let (_tmp, main) = tmp_main(&[
        ("math.c", "int add(int a, int b) { return a + b; }\n"),
        ("main.rfl", "import { add } from \"./math.c\";\nfn main() -> i16 { add(20, 22) }\n"),
    ]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn static_c_function_is_rejected() {
    let (_tmp, main) = tmp_main(&[
        ("secret.c", "static int secret(void) { return 1; }\n"),
        ("main.rfl", "import { secret } from \"./secret.c\";\nfn main() -> i16 { secret() }\n"),
    ]);
    let err = rfl::compile_file(&main).unwrap_err();
    assert!(err.to_string().contains("static"), "{err}");
}

#[test]
fn plain_struct_cannot_be_a_c_pointer() {
    let err = rfl::compile_to_ir(
        "struct Point { x: i16 }\nextern \"C\" { fn f(p: *mut Point); }\nfn main() -> i16 { 0 }\n",
    )
    .unwrap_err();
    assert!(err.to_string().contains("C signature"), "{err}");
}

#[test]
fn raw_pointer_cast_needs_unsafe() {
    let source = r#"
        #[repr(C)]
        struct Point { x: c_int, y: c_int }
        extern "C" { fn nudge(p: *mut Point, dx: c_int); }
        fn main() -> i16 {
            let mut p = Point { x: 1, y: 2 };
            nudge(&mut p, 1);
            p.x
        }
    "#;
    let err = rfl::compile_to_ir(source).unwrap_err();
    assert!(err.to_string().contains("unsafe"), "{err}");
}

#[test]
fn repr_c_struct_crosses_the_c_abi() {
    let (_tmp, main) = tmp_main(&[
        (
            "point.c",
            r#"
                struct Point { int x; int y; };
                struct Point make_point(int x, int y) {
                    struct Point p;
                    p.x = x;
                    p.y = y;
                    return p;
                }
                void nudge(struct Point *p, int dx) { p->x = p->x + dx; }
                int sum_point(struct Point p) { return p.x + p.y; }
            "#,
        ),
        (
            "main.rfl",
            r#"
                import { make_point, nudge, sum_point } from "./point.c";
                #[repr(C)]
                struct Point { x: c_int, y: c_int }
                fn main() -> i16 {
                    let mut p = make_point(10, 22);
                    unsafe { nudge(&mut p, 10); }
                    sum_point(p)
                }
            "#,
        ),
    ]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn printf_takes_variadic_arguments() {
    let source = r#"
        extern "C" {
            fn printf(format: *const c_char, ...) -> c_int;
        }
        fn main() -> i16 {
            printf(c"Result: %d\n", 20);
            42
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.stdout, "Result: 20\n", "status {}", result.status);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn variadic_call_needs_the_named_arguments() {
    let err = rfl::compile_to_ir(
        "extern \"C\" { fn printf(format: *const c_char, ...) -> c_int; }\nfn main() -> i16 { printf() }\n",
    )
    .unwrap_err();
    assert!(err.to_string().contains("at least"), "{err}");
}

#[test]
fn variadic_call_rejects_a_vector() {
    let err = rfl::compile_to_ir(
        "extern \"C\" { fn printf(format: *const c_char, ...) -> c_int; }\nfn main() -> i16 { let v: Vec<i16> = []; printf(c\"%d\", v) }\n",
    )
    .unwrap_err();
    assert!(err.to_string().contains("variadic"), "{err}");
}

#[test]
fn mixed_program_prints_the_folded_list() {
    let (_tmp, main) = tmp_main(&[
        ("math.c", "int sum(int a, int b) { return a + b; }\n"),
        (
            "list.rfl",
            r#"
                export type List<T> = Nil | Cons(T, List<T>);
                export fn map(f, xs) {
                    match xs {
                        Nil => Nil,
                        Cons(x, rest) => Cons(f(x), map(f, rest)),
                    }
                }
                export fn fold(f, initial, xs) {
                    match xs {
                        Nil => initial,
                        Cons(x, rest) => fold(f, f(initial, x), rest),
                    }
                }
            "#,
        ),
        (
            "main.rfl",
            r#"
                import { List, map, fold } from "./list";
                import { sum } from "./math.c";
                extern "C" {
                    fn printf(format: *const c_char, ...) -> c_int;
                }
                fn main() -> c_int {
                    let numbers = Cons(1, Cons(2, Cons(3, Cons(4, Nil))));
                    let doubled = map(fn(x) => sum(x, x), numbers);
                    let total = fold(fn(acc, x) => acc + x, 0, doubled);
                    printf(c"Result: %d\n", total);
                    0
                }
            "#,
        ),
    ]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.stdout, "Result: 20\n", "status {}", result.status);
    assert_eq!(result.status, 0, "{}", result.stdout);
}

#[test]
fn std_list_chains_and_prints() {
    let (_tmp, main) = tmp_main(&[(
        "main.rfl",
        r#"
            import { List } from "std/list";
            import { println } from "std/io";
            fn main() -> i16 {
                let numbers = List::from([1, 2, 3, 4, 5]);
                let result = numbers
                    .map(fn(x) => x * 2)
                    .filter(fn(x) => x > 5)
                    .fold(0, fn(acc, x) => acc + x);
                println(result);
                if (result == 24) { 42 } else { result }
            }
        "#,
    )]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.stdout, "24\n", "status {}", result.status);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn imported_function_is_a_method() {
    let (_tmp, main) = tmp_main(&[(
        "main.rfl",
        r#"
            import { abs } from "std/math";
            fn main() -> i16 {
                let n = -41;
                if (n.abs() == 41 && abs(n) == 41) { 42 } else { 0 }
            }
        "#,
    )]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn std_option_is_the_prelude() {
    let (_tmp, main) = tmp_main(&[(
        "main.rfl",
        r#"
            import { Option, unwrap_or } from "std/option";
            fn pick(flag: bool) -> Option<i16> {
                if (flag) { Some(42) } else { None }
            }
            fn chosen() -> Option<i16> {
                let missing = pick(false);
                let value = pick(true)?;
                Some(unwrap_or(missing, value))
            }
            fn main() -> i16 {
                match chosen() {
                    Some(n) => n,
                    None => 0,
                }
            }
        "#,
    )]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn std_result_unwraps_or_the_fallback() {
    let (_tmp, main) = tmp_main(&[(
        "main.rfl",
        r#"
            import { unwrap_or } from "std/result";
            fn main() -> i16 {
                let bad: Result<i16, i16> = Err(1);
                let good: Result<i16, i16> = Ok(42);
                if (unwrap_or(bad, 7) == 7 && unwrap_or(good, 7) == 42) { 42 } else { 0 }
            }
        "#,
    )]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn std_string_reports_byte_length() {
    let (_tmp, main) = tmp_main(&[(
        "main.rfl",
        "import { byte_len } from \"std/string\";\nfn main() -> i16 { if (byte_len(\"Hi\") == 2) { 42 } else { 0 } }\n",
    )]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn return_inside_a_loop_stores_the_value() {
    let source = r#"
        fn find_e(text: String) -> bool {
            let mut i: usize = 0;
            while (i < text.len()) {
                if (text[i] == 'e') { return true; }
                i = i + 1;
            }
            false
        }
        fn early(n: i16) -> i16 {
            if (n > 0) { return n; }
            0
        }
        fn main() -> i16 {
            if (find_e("hello") && early(42) == 42) { 42 } else { 0 }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn bitwise_packs_and_shifts() {
    let source = r#"
        fn main() -> i16 {
            let low: u16 = 15;
            let high: u16 = 60;
            let packed = (low & high) | (1 << 8);
            let negative: i16 = -8;
            if (packed == 268 && (negative >> 1) == -4 && (~low & 15) == 0) { 42 } else { 0 }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn mutable_capture_is_shared() {
    let source = r#"
        fn make() -> fn() -> i16 {
            let mut n = 40;
            fn() => {
                n = n + 1;
                n
            }
        }
        fn main() -> i16 {
            let mut seen = 1;
            let bump = fn() => { seen = seen + 1; };
            bump();
            let next = make();
            let first = next();
            let second = next();
            if (seen == 2 && first == 41 && second == 42) { 42 } else { 0 }
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn c_function_pointer_calls_through_c() {
    let (_tmp, main) = tmp_main(&[
        ("cb.c", "int call_cb(int (*f)(int, int), int a, int b) { return f(a, b); }\n"),
        (
            "main.rfl",
            r#"
                import { call_cb } from "./cb.c";
                export extern "C" fn add(a: c_int, b: c_int) -> c_int { a + b }
                fn apply(f: extern "C" fn(c_int, c_int) -> c_int, a: c_int, b: c_int) -> c_int { f(a, b) }
                fn main() -> i16 {
                    if (apply(add, 10, 11) == 21 && call_cb(add, 20, 22) == 42) { 42 } else { 0 }
                }
            "#,
        ),
    ]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn native_function_is_not_a_c_pointer() {
    let err = rfl::compile_to_ir(
        "fn add(a: i16, b: i16) -> i16 { a + b }\nfn main() -> i16 {\n    let f: extern \"C\" fn(i16, i16) -> i16 = add;\n    f(1, 2)\n}\n",
    )
    .unwrap_err();
    assert!(err.to_string().contains("extern"), "{err}");
}

#[test]
fn raw_pointer_updates_a_local() {
    let source = r#"
        import { write_u16, write_at } from "std/mem";
        fn main() -> i16 {
            let mut x: u16 = 20;
            unsafe {
                let p: *mut u16 = &mut x;
                *p = *p + 22;
                let q = p.offset(0);
                let same = q.addr();
                let here = p.addr();
                if (q.is_null() || same != here) { return 0; }
                write_u16(p, 40);
                write_at(p, 0, 42);
            }
            if (x == 42) { 42 } else { 0 }
        }
    "#;
    let (_tmp, main) = tmp_main(&[("main.rfl", source)]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn raw_pointer_needs_unsafe() {
    let err = rfl::compile_to_ir("fn load(p: *mut u16) -> u16 { *p }\nfn main() -> i16 { 0 }\n").unwrap_err();
    assert!(err.to_string().contains("unsafe"), "{err}");
}

#[test]
fn const_pointer_cannot_store() {
    let err = rfl::compile_to_ir("fn store(p: *const u16) { unsafe { *p = 1; } }\nfn main() -> i16 { 0 }\n").unwrap_err();
    assert!(err.to_string().contains("const"), "{err}");
}

#[test]
fn mmio_talks_to_the_devices() {
    let (_tmp, main) = tmp_main(&[(
        "main.rfl",
        r#"
            import { tty_put, tty_write, set_mode, mode, MODE_TEXT40, rng_reseed, rng_seed, device_version } from "std/mmio";
            fn main() -> i16 {
                tty_put(89);
                tty_write("Z");
                set_mode(MODE_TEXT40);
                rng_reseed(7);
                if (mode() == MODE_TEXT40 && rng_seed() == 7 && device_version() == 1) { 42 } else { 0 }
            }
        "#,
    )]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.stdout, "YZ", "status {}", result.status);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn std_math_covers_integers_and_trig() {
    let (_tmp, main) = tmp_main(&[(
        "main.rfl",
        r#"
            import { abs, gcd, lcm, isqrt, pow, count_ones, clamp, signum, min, sin, cos, sqrt, floor, ceil } from "std/math";
            fn near(actual: f64, expected: f64) -> bool {
                let gap = if (actual > expected) { actual - expected } else { expected - actual };
                gap < 0.001
            }
            fn main() -> i16 {
                let words = abs(-3) == 3 && signum(-2) == -1 && gcd(12, 18) == 6 && lcm(4, 6) == 12
                    && isqrt(10) == 3 && pow(2, 5) == 32 && count_ones(7) == 3 && clamp(9, 1, 4) == 4
                    && min(1.5, 2.5) == 1.5;
                let curve = near(sin(0.0), 0.0) && near(cos(0.0), 1.0) && near(sin(1.5707963267948966), 1.0)
                    && near(sqrt(4.0), 2.0) && floor(-1.2) == -2.0 && ceil(1.2) == 2.0;
                if (words && curve) { 42 } else { 0 }
            }
        "#,
    )]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn std_list_option_string_and_char() {
    let (_tmp, main) = tmp_main(&[(
        "main.rfl",
        r#"
            import { List } from "std/list";
            import { map } from "std/option";
            import { contains, starts_with } from "std/string";
            import { to_lower, digit } from "std/char";
            fn positive(n: i16) -> bool { n > 0 }
            fn twice(n: i16) -> i16 { n * 2 }
            fn main() -> i16 {
                let items = List::from([1, 2, 3]);
                let flipped = items.reverse();
                let found = match flipped.find(positive) {
                    Some(n) => n,
                    None => 0,
                };
                let doubled = match map(Some(21), twice) {
                    Some(n) => n,
                    None => 0,
                };
                let tail = match flipped.last() {
                    Some(n) => n,
                    None => 0,
                };
                let count = flipped.len();
                let text_ok = contains("hello", "ell") && starts_with("hello", "he") && !contains("hello", "hex");
                let letter = to_lower('B');
                let value = match digit('7') { Some(n) => n, None => 0 };
                if (count == 3 && found == 3 && tail == 1 && doubled == 42
                    && text_ok && letter == 'b' && value == 7) { 42 } else { 0 }
            }
        "#,
    )]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn std_mem_copies_cells() {
    let (_tmp, main) = tmp_main(&[(
        "main.rfl",
        r#"
            import { copy, fill, read_i32, write_i32 } from "std/mem";
            fn main() -> i16 {
                let mut cell: u16 = 7;
                let mut out: u16 = 0;
                let mut wide: i32 = 0;
                unsafe { copy(&mut out, &cell, 1); }
                let copied = out;
                unsafe { fill(&mut out, 1, 9); }
                unsafe { write_i32(&mut wide, 65537); }
                let got = unsafe { read_i32(&wide) };
                if (copied == 7 && out == 9 && got == 65537) { 42 } else { 0 }
            }
        "#,
    )]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn strings_can_be_built_and_sliced() {
    let (_tmp, main) = tmp_main(&[(
        "main.rfl",
        r#"
            import { trim, repeat, replace, split } from "std/string";
            import { List } from "std/list";
            fn main() -> i16 {
                let program = "++++++++++[->++++++<]>+++++.";
                println(program[0]);
                let joined = "a" + "b";
                let piece = "hello"[1..=3];
                let built = from_char('+') + from_byte(43);
                let mut walked = "";
                for (ch in "ab") { walked = walked + from_char(ch); }
                let parts = split("a,b", ",");
                let count = parts.len();
                let first = match parts.nth(0) { Some(s) => s, None => "" };
                let second = match parts.nth(1) { Some(s) => s, None => "" };
                if (joined == "ab" && piece == "ell" && built == "++" && from_char('é') == "é" && walked == "ab"
                    && trim("  hi ") == "hi" && repeat("ab", 3) == "ababab"
                    && replace("aba", "a", "c") == "cbc" && count == 2 && first == "a" && second == "b") {
                    42
                } else { 0 }
            }
        "#,
    )]);
    let result = rfl::run_file(&main).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.stdout, "+\n", "status {}", result.status);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn match_can_test_a_character() {
    let source = r#"
        fn main() -> i16 {
            let program = "+";
            match program[0] {
                '+' => { println("plus"); }
                _ => { println("other"); }
            }
            42
        }
    "#;
    let result = rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.stdout, "plus\n", "status {}", result.status);
    assert_eq!(result.status, 42, "{}", result.stdout);
}
