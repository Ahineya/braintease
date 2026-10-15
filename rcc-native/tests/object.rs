use rcc_ir::ir::*;
#[test]
fn c_style_external_calls_require_explicit_runtime_bindings() {
    let mut builder = IrBuilder::new();
    builder.create_function("main".into(), IrType::I16);
    builder.create_block(0).unwrap();
    builder
        .build_call(
            Value::Global("putchar".into()),
            vec![Value::Constant(89)],
            IrType::Void,
        )
        .unwrap();
    builder.build_return(Some(Value::Constant(0))).unwrap();
    let mut module = Module::new("external".into());
    module.target = rcc_ir::target::Target::Native;
    module.add_function(builder.finish_function().unwrap());
    assert!(rcc_native::compile_object(&module).is_ok());
}

#[test]
fn target_and_unsupported_external_calls_fail_explicitly() {
    let mut builder = IrBuilder::new();
    builder.create_function("main".into(), IrType::I16);
    builder.create_block(0).unwrap();
    builder
        .build_call(
            Value::Global("printf".into()),
            vec![Value::Constant(0)],
            IrType::Void,
        )
        .unwrap();
    builder.build_return(Some(Value::Constant(0))).unwrap();
    let mut module = Module::new("unsupported".into());
    module.add_function(builder.finish_function().unwrap());
    assert!(rcc_native::compile_object(&module)
        .unwrap_err()
        .contains("--target native"));
    module.target = rcc_ir::target::Target::Native;
    assert!(rcc_native::compile_object(&module)
        .unwrap_err()
        .contains("no compatible runtime binding"));
    assert!(rcc_backend::lower_module_to_assembly_with_options(
        &module,
        rcc_backend::LoweringOptions::default()
    )
    .unwrap_err()
    .contains("native IR"));
}
