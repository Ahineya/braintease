fn run(source: &str) -> rfl::RunResult {
    rfl::run_program(source).unwrap_or_else(|err| panic!("{err}"))
}

#[test]
fn invariant_float_expressions_do_not_call_softfloat_at_runtime() {
    use rcc_ir::ir::{Instruction, Value};
    let module = rfl::compile_to_ir(
        r#"
        fn calc() -> f64 {
            let rate: u32 = 44100;
            let frequency = 440.0;
            2.0 * 3.141592653589793 * frequency / (rate as f64)
        }
        fn main() { println(calc()); }
    "#,
    )
    .unwrap();
    let calc = module
        .functions
        .iter()
        .find(|f| f.name == "rfl_fn_calc")
        .unwrap();
    assert!(
        !calc
            .blocks
            .iter()
            .flat_map(|b| &b.instructions)
            .any(|i| matches!(i,
        Instruction::Call { function: Value::Global(name), .. } if name.starts_with("__rcc_f"))),
        "constant arithmetic still calls the software-float runtime"
    );
}

#[test]
fn constant_f64_results_agree_with_runtime_arithmetic() {
    let result = run(r#"
        fn add(a: f64, b: f64) -> f64 { a + b }
        fn sub(a: f64, b: f64) -> f64 { a - b }
        fn mul(a: f64, b: f64) -> f64 { a * b }
        fn div(a: f64, b: f64) -> f64 { a / b }
        fn main() -> i16 {
            if (0.1 + 0.2 != add(0.1, 0.2)) { return 1; }
            if (1.0000000000000002 - 1.0 != sub(1.0000000000000002, 1.0)) { return 2; }
            if (3.141592653589793 * 440.0 != mul(3.141592653589793, 440.0)) { return 3; }
            if (1.0 / 3.0 != div(1.0, 3.0)) { return 4; }
            if (-2.0 / 7.0 != div(-2.0, 7.0)) { return 5; }
            if (1.0e150 * 1.0e150 != mul(1.0e150, 1.0e150)) { return 6; }
            if (1.0e-150 * 1.0e-150 != mul(1.0e-150, 1.0e-150)) { return 7; }
            let rate: u32 = 4294967295;
            if ((rate as f64) != 4294967295.0) { return 8; }
            42
        }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn mutable_and_borrowed_floats_keep_runtime_values_across_calls() {
    let result = run(r#"
        fn alter(p: *mut f64) { unsafe { *p = 10.5; } }
        fn identity(x: f64) -> f64 { x }
        fn main() -> i16 {
            let mut x = 1.5;
            let mut n = 0;
            while (n < 4) { x = identity(x + 0.25); n += 1; }
            if (x != 2.5) { return 1; }
            unsafe { alter(&mut x); }
            if (x != 10.5) { return 2; }
            42
        }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn float_formatting_preserves_rounding_signs_and_fractional_zeroes() {
    let result = run(r#"
        fn main() {
            println(0.0); println(-0.0); println(0.5); println(0.000001);
            println(0.0001); println(0.0001005); println(1.2345678);
            println(9.9999999); println(-42.00001); println(9999999.999999);
            println(10000000.0); println(1.0e20); println(1.0e-20);
            println(0.0 / 0.0); println(1.0 / 0.0); println(-1.0 / 0.0);
            let n: u64 = 1000000000 * 10000000000;
            println(n); println(0); println(9); println(10);
        }
    "#);
    assert_eq!(result.status, 0, "{}", result.stdout);
    assert_eq!(
        result.stdout,
        concat!(
            "0\n0\n0.5\n1e-6\n0.0001\n0.000101\n1.234568\n10\n-42.00001\n",
            "9999999.999999\n1e+7\n1e+20\n1e-20\nnan\ninf\n-inf\n",
            "10000000000000000000\n0\n9\n10\n"
        )
    );
}
