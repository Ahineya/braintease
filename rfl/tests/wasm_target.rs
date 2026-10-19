use rfl::{compile_to_ir_for_target, Target};

#[test]
fn wasm_cfg_uses_caller_target_and_does_not_enable_js_math() {
    let source = r#"
        #[cfg(all(target = "wasm", target_arch = "wasm32",
                  target_has = "native_float", not(target_has = "host_math")))]
        fn answer() -> i16 { 42 }
        #[cfg(any(target = "native", target = "ripple", target_has = "host_math"))]
        fn answer() -> i16 { invalid_non_wasm_symbol() }
        fn main() -> i16 { answer() }
    "#;
    let module = compile_to_ir_for_target(source, Target::Wasm).unwrap();
    assert_eq!(module.target, Target::Wasm);
    assert_eq!(
        rcc_ir::parse_module(&rcc_ir::emit_module(&module)).unwrap(),
        module
    );
}
