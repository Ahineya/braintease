use rcc_ir::ir::*;

fn program() -> Module {
    let mut builder = IrBuilder::new();
    builder.create_function("main".into(), IrType::I16);
    builder.create_block(0).unwrap();
    builder.build_return(Some(Value::Constant(42))).unwrap();
    let mut module = Module::new("cross-kernel".into());
    module.target = rcc_ir::target::Target::Native;
    module.add_function(builder.finish_function().unwrap());
    module
}

#[test]
fn explicit_freestanding_target_emits_x86_elf_on_any_build_host() {
    let object =
        rcc_native::compile_freestanding_object(&program(), "x86_64-unknown-none").unwrap();
    assert_eq!(&object[..6], b"\x7fELF\x02\x01");
    assert_eq!(&object[18..20], &[62, 0]);
    assert!(object
        .windows(b"rcc_native_entry".len())
        .any(|w| w == b"rcc_native_entry"));
    assert!(!object
        .windows(b"rcc_native_start".len())
        .any(|w| w == b"rcc_native_start"));
}

#[test]
fn invalid_freestanding_targets_and_ripple_ir_fail_explicitly() {
    assert!(
        rcc_native::compile_freestanding_object(&program(), "invalid-target")
            .unwrap_err()
            .contains("x86_64-unknown-none")
    );
    let mut module = program();
    module.target = rcc_ir::target::Target::Ripple;
    assert!(
        rcc_native::compile_freestanding_object(&module, "x86_64-unknown-none")
            .unwrap_err()
            .contains("--target native")
    );
}

#[test]
fn freestanding_application_has_target_identity_and_no_kernel_entry() {
    let mut module = program();
    module.functions[0].parameters.push((1, IrType::I32));
    let (object, manifest) =
        rcc_native::compile_freestanding_application(&module, "x86_64-unknown-none").unwrap();
    assert_eq!(&object[18..20], &[62, 0]);
    assert_eq!(
        (manifest.arch.as_str(), manifest.os.as_str()),
        ("x86_64", "none")
    );
    assert!(manifest
        .functions
        .iter()
        .any(|f| f.token == manifest.entry && f.arity == 1));
    assert!(!object
        .windows(b"rcc_native_entry".len())
        .any(|w| w == b"rcc_native_entry"));
}
