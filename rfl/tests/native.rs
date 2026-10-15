//! Native end-to-end checks use the real AOT object linker and Rust runtime.
//! Build the runtime first: cargo build --release -p rcc-native-runtime.
use rfl::{run_native_program, Target};
fn run(source: &str) -> rfl::RunResult {
    run_native_program(source).unwrap_or_else(|e| panic!("{e}"))
}
#[test]
fn scalar_widths_phis_and_hardware_floats() {
    let result = run(r#"
        fn main() -> i16 {
            let a: i16 = -32768;
            let minus: i16 = -1;
            let u: u32 = 4294967295;
            println(a / minus);
            println(u + 1);
            let mut total: i32 = 0;
            for (i in 0..10) { total += i as i32; }
            println(total);
            let x: f32 = 1.25;
            let y = (x as f64) * 2.0 + 0.5;
            println(y);
            if (y == 3.0 && (y as i64) == 3) { 42 } else { 1 }
        }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
    assert_eq!(result.stdout, "-32768\n0\n45\n3\n");
}
#[test]
fn managed_strings_arrays_slices_and_cow_vectors() {
    let result = run(r#"
        fn view() -> Slice<String> {
            let fixed: [String; 2] = ["left", "right"];
            fixed[0..2]
        }
        fn main() -> i16 {
            let mut values: Vec<String> = ["one", "two"];
            let prior = values;
            values[0] = "changed";
            values.push("three");
            let fixed: [String; 2] = ["A", "B"];
            let slice = view();
            println(values[0]);
            if (prior[0] == "one" && slice[1] == "right" && fixed[1] == "B" && values.len() == 3) { 42 } else { 1 }
        }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
    assert_eq!(result.stdout, "changed\n");
}
#[test]
fn closures_and_deep_tail_calls_keep_the_frame_protocol() {
    let result = run(r#"
        fn countdown(n: u32, result: i16) -> i16 {
            if (n == 0) { result } else { countdown(n - 1, result) }
        }
        fn make(offset: i16) -> fn(i16) -> i16 { fn(x: i16) => x + offset }
        fn main() -> i16 {
            let f = make(40);
            countdown(100000, f(2))
        }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}
#[test]
fn native_faults_return_nonzero_with_a_reason() {
    for source in [
        "fn main() { let xs: Vec<i16> = []; println(xs[0]); }",
        "fn divide(n: i16) -> i16 { 4 / n } fn main() { println(divide(0)); }",
    ] {
        let result = run(source);
        assert_eq!(result.status, 1, "{}", result.stdout);
        assert!(!result.stdout.is_empty());
    }
}
#[test]
fn cfg_and_float_ir_do_not_select_softfloat_on_native() {
    let source = r#"#[cfg(target_has = "host_math")] fn value() -> f64 { 1.25 }
        #[cfg(not(target_has = "host_math"))] fn value() -> f64 { missing(); 0.0 }
        fn main() { println(value() + 2.0); }"#;
    let module = rfl::compile_to_ir_for_target(source, Target::Native).unwrap();
    let text = rcc_ir::emit_module(&module);
    assert!(text.contains("intrinsic f64 @float.f64.add"));
    assert!(!text.contains("__rcc_f"));
    assert_eq!(run(source).stdout, "3.25\n");
}

struct Fixture(std::path::PathBuf);
impl Fixture {
    fn new(files: &[(&str, &str)]) -> Self {
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = std::env::temp_dir().join(format!("rfl-native-{}-{stamp}", std::process::id()));
        std::fs::create_dir_all(&root).unwrap();
        for (name, source) in files {
            std::fs::write(root.join(name), source).unwrap();
        }
        Self(root)
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
#[test]
fn native_c_and_rfl_share_callbacks_and_floating_values() {
    let fixture=Fixture::new(&[
        ("math.c", "int call_cb(int (*f)(int, int), int a, int b) { return f(a,b); } double scale(double x) { return x * 2.0; }"),
        ("main.rfl", r#"
            import {call_cb, scale} from "./math.c";
            export extern "C" fn add(a: c_int, b: c_int) -> c_int { a + b }
            fn main() -> i16 {
                println(scale(1.75));
                call_cb(add, 20, 22)
            }
        "#),
    ]);
    let result = rfl::run_native_file(&fixture.0.join("main.rfl"), false).unwrap();
    assert_eq!(result.status, 42);
    assert_eq!(result.stdout, "3.5\n");
}
#[test]
fn host_math_imports_and_nested_target_gating() {
    let fixture = Fixture::new(&[
        (
            "main.rfl",
            r#"
            #[cfg(target = "ripple")] import {missing} from "./does-not-exist";
            import {answer} from "./selected";
            import {sin, cos, sqrt, floor, PI} from "std/math";
            fn main() -> i16 {
                if (sin(PI / 2.0) > 0.999999 && cos(PI) < -0.999999 && sqrt(9.0) == 3.0 && floor(-1.1) == -2.0) { answer() } else { 1 }
            }
        "#,
        ),
        (
            "selected.rfl",
            r#"
            #[cfg(target_has = "host_math")] export fn answer() -> i16 { 42 }
            #[cfg(not(target_has = "host_math"))] export fn answer() -> i16 { missing(); 0 }
        "#,
        ),
    ]);
    assert_eq!(
        rfl::run_native_file(&fixture.0.join("main.rfl"), false)
            .unwrap()
            .status,
        42
    );
}
#[test]
fn native_rgb565_first_frame_contains_the_expected_pixels() {
    let fixture = Fixture::new(&[(
        "main.rfl",
        r#"
        import {display_size, rgb565, write, display_flush} from "std/mmio";
        fn main() {
            display_size(2,2); rgb565();
            write(36,63488); write(37,2016); write(38,31); write(39,65535);
            display_flush();
        }
    "#,
    )]);
    let executable = fixture.0.join("pixels");
    let frame = fixture.0.join("frame.rgb565");
    rfl::compile_file_to_native(&fixture.0.join("main.rfl"), &executable).unwrap();
    let result = std::process::Command::new(executable)
        .env("RCC_NATIVE_FRAME", &frame)
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{}",
        String::from_utf8_lossy(&result.stderr)
    );
    assert_eq!(
        std::fs::read(frame).unwrap(),
        [63488u16, 2016, 31, 65535]
            .into_iter()
            .flat_map(u16::to_le_bytes)
            .collect::<Vec<_>>()
    );
}
#[test]
fn narrow_signed_comparisons_and_cross_bank_managed_memory() {
    let result = run(r#"
        fn eq(n: i8) -> bool { n == -1 && n < 0 }
        fn main() -> i16 {
            let mut values: Vec<i16> = [];
            for (i in 0..70000) { values.push((i as u32 % 97) as i16); }
            if (eq(-1) && values[65536] == 61 && values[69999] == 62) { 42 } else { 1 }
        }
    "#);
    assert_eq!(result.status, 42, "{}", result.stdout);
}

#[test]
fn floating_special_values_and_saturating_integer_conversions() {
    let source = r#"
        fn check(zero: f64, huge: f64) -> i16 {
            let nan = zero / zero;
            let inf = 1.0 / zero;
            let negative_zero = -zero;
            if (nan == nan || nan < zero || !(nan != nan)) { return 1; }
            if (1.0 / negative_zero != -inf) { return 2; }
            if ((nan as i32) != 0 || (inf as u32) != 4294967295) { return 3; }
            if ((huge as i32) != 2147483647 || ((-huge) as i32) != -2147483648) { return 4; }
            let maximum: u64 = 9223372036854775807 * 2 + 1;
            if ((huge as u64) != maximum || ((-huge) as u64) != 0) { return 5; }
            42
        }
        fn main() -> i16 { check(0.0,1.0e300) }
    "#;
    assert_eq!(run(source).status, 42);
    // The compatibility target must retain the same conversion rules.
    assert_eq!(rfl::run_program(source).unwrap().status, 42);
}

#[test]
fn c_global_function_pointers_can_reference_runtime_bindings() {
    let fixture=Fixture::new(&[
        ("emit.c", "extern void putchar(int); void (*out)(int) = putchar; int show(void) { out(89); return 42; }"),
        ("main.rfl", "import {show} from \"./emit.c\"; fn main() -> i16 { show() }"),
    ]);
    let result = rfl::run_native_file(&fixture.0.join("main.rfl"), false).unwrap();
    assert_eq!(result.status, 42);
    assert_eq!(result.stdout, "Y");
}
