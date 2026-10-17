#[test]
fn long_literals_use_data_instead_of_per_byte_instructions() {
    use rcc_ir::ir::Instruction;
    let literal = "abcdef0123456789".repeat(64);
    let source =
        format!("fn text() -> String {{ \"{literal}\" }} fn main() {{ println(text()); }}");
    let module = rfl::compile_to_ir(&source).unwrap();
    let function = module
        .functions
        .iter()
        .find(|f| f.name == "rfl_fn_text")
        .unwrap();
    let stores = function
        .blocks
        .iter()
        .flat_map(|b| &b.instructions)
        .filter(|i| matches!(i, Instruction::Store { .. }))
        .count();
    assert!(stores < 100, "literal generated {stores} stores");
}

#[test]
fn pooled_literals_preserve_bytes_lifetimes_and_aliases_on_both_targets() {
    let source = r#"
        extern "C" { fn puts(text: *const i16) -> i16; }
        fn literal() -> String { "hello UTF-8: café" }
        fn main() -> i16 {
            let mut value = literal();
            let alias = value;
            value = value + "!";
            if (alias != literal() || value != "hello UTF-8: café!") { return 1; }
            let nul = "before\0after";
            if (nul.len() != 12 || nul[6] != '\0' || nul[7..12] != "after") { return 2; }
            for (_ in 0..8) { let text = literal(); if (text != alias) { return 3; } }
            puts(c"pooled ASCII literal");
            println("pooled ASCII literal");
            println(alias);
            42
        }
    "#;
    for result in [rfl::run_program(source), rfl::run_native_program(source)] {
        let result = result.unwrap();
        assert_eq!(result.status, 42, "{}", result.stdout);
        assert_eq!(
            result.stdout,
            "pooled ASCII literal\npooled ASCII literal\nhello UTF-8: café\n"
        );
    }
}
