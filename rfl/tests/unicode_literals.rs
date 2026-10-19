#[test]
fn supplementary_unicode_literals_are_scalars_and_utf8_strings() {
    let source = r#"fn main() -> i16 {
        let text = "🙂Ж";
        if (text.len() != 6 || text[0..4] != from_char('🙂') || text[4..6] != "Ж") { return 1; }
        println(text); 0
    }"#;
    for result in [rfl::run_program(source), rfl::run_native_program(source)] {
        let result = result.unwrap();
        assert_eq!(result.status, 0);
        assert_eq!(result.stdout, "🙂Ж\n");
    }
}
