#[test]
fn sliced_strings_infer_concatenation_and_comparison() {
    let source = r#"
        fn main() -> i16 {
            let mut value = "abcd";
            let cursor: usize = 2;
            value = value[0..cursor - 1] + value[cursor..value.len()];
            let inferred = value[0..1] + value[1..3];
            if (inferred == "acd" && value[0..2] == inferred[0..2]) { 42 } else { 1 }
        }
    "#;
    for result in [rfl::run_program(source), rfl::run_native_program(source)] {
        let result = result.unwrap();
        assert_eq!(result.status, 42, "{}", result.stdout);
    }
}
