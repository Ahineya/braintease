#[test]
fn raw_pointer_indexing_preserves_numeric_type_hints() {
    let source = include_str!("fixtures/pointer-index-inference.rfl");
    let result = rfl::run_wasm_program(source).unwrap();
    assert_eq!(result.status, 0);
}
