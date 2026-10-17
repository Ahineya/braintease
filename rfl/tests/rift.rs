//! The actual RFL vi controller and C engine. Both guest ABIs are also exercised
//! by tools/test_rift.py; keep this standalone test on the native target.
#[test]
fn vi_editor_and_text_engine_run_natively() {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/rift.rfl");
    let result = rfl::run_native_file(&path, false).unwrap_or_else(|error| panic!("{error}"));
    assert_eq!(result.status, 0, "{}", result.stdout);
    assert!(result.stdout.contains("RIFT_CORE_OK"), "{}", result.stdout);
}
