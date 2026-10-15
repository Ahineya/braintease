fn run(source: &str) -> rfl::RunResult {
    rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"))
}

#[test]
fn functional_brainfuck_example_runs() {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("examples/bf-fn.rfl");
    let result = rfl::run_file(&path).unwrap_or_else(|err| panic!("{err}"));
    assert_eq!(result.stdout, "A");
    assert_eq!(result.status, 0);
}

#[test]
fn dropping_adts_does_not_corrupt_live_values() {
    let result = run(r#"
        type List = | Nil | Cons(i16, List);
        fn make(n: i16) -> List {
            if (n == 0) { Nil } else { Cons(n, make(n - 1)) }
        }
        fn sum(xs: List) -> i16 {
            match xs { Nil => 0, Cons(n, rest) => n + sum(rest) }
        }
        fn churn() {
            let mut i = 0;
            while (i < 200) { make(20); i += 1; }
        }
        fn main() -> i16 {
            let keep = make(8);
            churn();
            sum(keep)
        }
    "#);
    assert_eq!(result.status, 36, "{}", result.stdout);
}

#[test]
fn argument_values_are_snapshots_in_left_to_right_order() {
    let result = run(r#"
        fn change(x: &mut Vec<i16>) -> i16 { x[0] = 99; 0 }
        fn first(x: Vec<i16>, ignored: i16) -> i16 { x[0] }
        fn main() -> i16 {
            let mut xs = [42];
            first(xs, change(&mut xs))
        }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn tail_calls_keep_borrowed_locals_alive() {
    let result = run(r#"
        fn read(xs: &Vec<i16>) -> i16 { xs[0] }
        fn owner() -> i16 { let xs = [42]; read(&xs) }
        fn main() -> i16 { owner() }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn nested_patterns_can_jointly_cover_a_constructor() {
    let result = run(r#"
        fn value(x: Option<Option<i16>>) -> i16 {
            match x {
                None => 1,
                Some(None) => 2,
                Some(Some(n)) => n,
            }
        }
        fn main() -> i16 { value(Some(Some(42))) }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn early_return_match_arms_do_not_require_a_unit_result() {
    let result = run(r#"
        fn value(x: Option<i16>) -> i16 {
            match x { None => { return 7; }, Some(n) => n }
        }
        fn main() -> i16 { value(None) + value(Some(35)) }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn scalar_bindings_survive_calls_and_borrowed_writes() {
    let result = run(r#"
        fn increment(p: *mut i16) { unsafe { *p = *p + 1; } }
        fn id(x: i16) -> i16 { x }
        fn main() -> i16 {
            let mut sum = 0;
            let mut i = 0;
            while (i < 6) { sum += id(i); i += 1; }
            unsafe { increment(&mut sum); }
            sum + 26
        }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn c_exports_keep_their_root_result_when_forwarding_a_call() {
    let result = run(r#"
        fn answer() -> i16 { 42 }
        export extern "C" fn relay() -> c_int { answer() }
        fn main() -> i16 { relay() }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn scalar_operand_snapshot_survives_a_suspending_call() {
    let result = run(r#"
        fn change(p: *mut i16) -> i16 { unsafe { *p = 99; } 0 }
        fn main() -> i16 {
            let mut x = 42;
            unsafe { x + change(&mut x) }
        }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn nested_pattern_holes_and_shadowed_arms_are_rejected() {
    for source in [
        "fn main() { match Some(Some(1)) { None => {}, Some(None) => {} } }",
        "fn main() { match Some(Some(1)) { Some(Some(_)) => {}, Some(Some(1)) => {}, _ => {} } }",
        "fn main() { match (true, false) { (true, _) => {}, (false, true) => {} } }",
    ] {
        assert!(rfl::check_source(source).is_err(), "{source}");
    }
}

#[test]
fn checking_a_library_does_not_require_an_executable_entry() {
    rfl::check_source("export fn answer() -> i16 { 42 }").unwrap();
    assert!(rfl::compile_to_ir("export fn answer() -> i16 { 42 }").is_err());
}

#[test]
fn unused_functions_do_not_enter_the_runtime_dispatcher() {
    let module = rfl::compile_to_ir(
        r#"
        fn unused() -> i16 { println("unreachable"); 99 }
        fn main() -> i16 { 42 }
    "#,
    )
    .unwrap();
    assert!(!module.functions.iter().any(|f| f.name == "rfl_fn_unused"));
    // Elimination follows checking: an unused invalid body remains an error.
    assert!(rfl::compile_to_ir("fn unused() -> i16 { true } fn main() {}").is_err());
}

#[test]
fn frame_scalars_reach_the_shared_ssa_optimizer() {
    use rcc_ir::ir::{Instruction, IrType};
    let mut module = rfl::compile_to_ir(
        r#"
        fn main() -> i16 {
            let mut i = 0;
            let mut sum = 0;
            while (i < 10) { sum += i; i += 1; }
            sum
        }
    "#,
    )
    .unwrap();
    rcc_backend::optimize::optimize_module(&mut module);
    let body = module
        .functions
        .iter()
        .find(|f| f.name == "rfl_fn_main")
        .unwrap();
    assert!(
        body.blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .any(|i| matches!(
                i,
                Instruction::Phi {
                    result_type: IrType::I16,
                    ..
                }
            )),
        "{}",
        rcc_ir::emit_module(&module)
    );
}

#[test]
fn string_operations_share_runtime_bodies() {
    use rcc_ir::ir::{Instruction, Value};
    let module = rfl::compile_to_ir(
        r#"
        fn main() {
            let a = "a" + "b";
            let b = a + "c";
            let c = b + "d";
            println(c);
        }
    "#,
    )
    .unwrap();
    assert_eq!(
        module
            .functions
            .iter()
            .filter(|f| f.name == "rfl_string_concat")
            .count(),
        1
    );
    let body = module
        .functions
        .iter()
        .find(|f| f.name == "rfl_fn_main")
        .unwrap();
    assert_eq!(
        body.blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .filter(|i| matches!(i,
        Instruction::Call { function: Value::Global(name), .. } if name == "rfl_string_concat"))
            .count(),
        3
    );
}

#[test]
fn inclusive_string_slices_check_index_overflow() {
    let result = run(r#"
        fn main() {
            let s = "abc";
            let end: usize = 4294967295;
            println(s[0..=end]);
        }
    "#);
    assert_eq!(result.status, 1, "{}", result.stdout);
    assert!(result.stdout.contains("overflow"), "{}", result.stdout);
}

#[test]
fn fixed_array_fields_use_element_width_not_total_array_width() {
    let result = run(r#"
        struct Tape { cells: [i16; 2], pos: i16 }
        fn total(t: Tape) -> i16 { t.cells[0] + t.cells[1] + t.pos }
        fn main() -> i16 { total(Tape { cells: [20, 21], pos: 1 }) }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn recursive_calls_in_both_value_and_unit_tails_do_not_suspend_the_caller() {
    use rcc_ir::ir::{Instruction, Value};
    for source in [
        "fn go(n: i16) -> i16 { if (n == 0) { 42 } else { go(n - 1) } } fn main() -> i16 { go(100) }",
        "fn go(n: i16) { if (n == 0) {} else { go(n - 1) } } fn main() { go(100); }",
    ] {
        let module = rfl::compile_to_ir(source).unwrap();
        let body = module.functions.iter().find(|f| f.name == "rfl_fn_go").unwrap();
        let resumes: Vec<_> = body.blocks.iter().flat_map(|b| &b.instructions).filter_map(|i| match i {
            Instruction::GetElementPtr { result, ptr: Value::FatPtr(p), indices, .. }
                if p.addr.as_ref() == &Value::Temp(0) && indices == &vec![Value::Constant(0)] => Some(*result),
            _ => None,
        }).collect();
        assert!(!body.blocks.iter().flat_map(|b| &b.instructions).any(|i| matches!(i,
            Instruction::Store { ptr: Value::FatPtr(p), value: Value::Constant(n), .. }
                if *n > 0 && matches!(p.addr.as_ref(), Value::Temp(id) if resumes.contains(id)))), "tail call saved a resume state: {source}");
    }
}

#[test]
fn character_collections_preserve_value_semantics() {
    let result = run(r#"
        fn main() -> i16 {
            let a = ['A', 'B'];
            let mut b = a;
            b[1] = 'C';
            b.push('D');
            let popped = b.pop();
            let fixed: [char; 2] = ['E', 'F'];
            if (a[1] == 'B' && b[1] == 'C' && fixed[1] == 'F') {
                match popped { Some('D') => 42, _ => 1 }
            } else { 0 }
        }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn imported_type_errors_point_to_the_original_file() {
    let unique = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let dir = std::env::temp_dir().join(format!("rfl-diagnostic-{}-{unique}", std::process::id()));
    std::fs::create_dir(&dir).unwrap();
    let lib = dir.join("lib.rfl");
    let main = dir.join("main.rfl");
    std::fs::write(&lib, "export fn bad() -> i16 {\n    true\n}\n").unwrap();
    std::fs::write(
        &main,
        "import { bad } from \"./lib\";\nfn main() -> i16 { bad() }\n",
    )
    .unwrap();
    let checked = rfl::check_file(&main).unwrap_err();
    let compiled = rfl::compile_file(&main).unwrap_err();
    std::fs::remove_dir_all(&dir).unwrap();
    for err in [checked, compiled] {
        assert_eq!(err.line, 2, "{err}");
        assert!(err.message.contains("lib.rfl:2:"), "{err}");
        assert!(err.message.contains("true"), "{err}");
    }
}

#[test]
fn unreachable_calls_do_not_create_missing_resume_labels() {
    let result = run(r#"
        fn unused() { println("unreachable"); }
        fn value(x: Option<i16>) -> i16 {
            match x { Some(n) => { return n; }, None => { return 0; } }
            unused();
            99
        }
        fn main() -> i16 { value(Some(42)) }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
    assert_eq!(result.stdout, "");
}
