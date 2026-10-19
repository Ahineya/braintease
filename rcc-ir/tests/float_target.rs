use rcc_ir::{
    float::{self, FloatIntrinsic},
    ir::*,
    target::Target,
};
#[test]
fn floats_round_trip_without_selecting_software_helpers() {
    let mut builder = IrBuilder::new();
    builder.create_function("main".into(), IrType::I32);
    builder.create_block(0).unwrap();
    let result = float::build_from_helper(
        &mut builder,
        "__rcc_f32_add",
        vec![
            Value::Constant(1.0f32.to_bits() as i64),
            Value::Constant(2.0f32.to_bits() as i64),
        ],
        IrType::I32,
    )
    .unwrap()
    .unwrap();
    builder.build_return(Some(Value::Temp(result))).unwrap();
    let mut module = Module::new("floats".into());
    module.add_function(builder.finish_function().unwrap());
    let text = rcc_ir::emit_module(&module);
    assert!(text.contains("intrinsic f32 @float.f32.add"));
    assert!(!text.contains("__rcc_"));
    assert_eq!(rcc_ir::parse_module(&text).unwrap(), module);
    float::lower_to_softfloat(&mut module).unwrap();
    let text = rcc_ir::emit_module(&module);
    assert!(text.contains("call f32 global @__rcc_f32_add"));
    assert!(!text.contains("intrinsic"));
}
#[test]
fn floating_signatures_and_capability_typos_fail_explicitly() {
    let spec = FloatIntrinsic::from_helper("__rcc_f64_from_u64").unwrap();
    assert_eq!(spec.params(), vec![IrType::I64]);
    assert_eq!(spec.result(), IrType::F64);
    assert!(spec.validate(&[], &IrType::F64).is_err());
    assert!(spec.validate(&[Value::Constant(0)], &IrType::I64).is_err());
    assert!(FloatIntrinsic::parse("float.f32.from_f32").is_none());
    assert!(Target::Native.has("host_mat").is_err());
    assert!(!Target::Ripple.has("host_math").unwrap());
    assert!(Target::Native.has("host_math").unwrap());
    assert_eq!(
        Target::Native
            .layout()
            .size_in_cells(&IrType::FatPtr(Box::new(IrType::I16)))
            .unwrap(),
        2
    );
    assert!(Target::Native
        .layout()
        .size_in_cells(&IrType::Array {
            size: u64::MAX,
            element_type: Box::new(IrType::I64)
        })
        .is_err());
}

#[test]
fn target_metadata_survives_text_and_json() {
    let mut module = Module::new("native".into());
    module.target = Target::Native;
    assert_eq!(
        rcc_ir::parse_module(&rcc_ir::emit_module(&module)).unwrap(),
        module
    );
    assert_eq!(
        serde_json::from_str::<Module>(&serde_json::to_string(&module).unwrap()).unwrap(),
        module
    );
    assert!(rcc_ir::parse_module("module native { target native target ripple }").is_err());
    assert!(rcc_ir::parse_module("module native { target unknown }").is_err());
}

#[test]
fn wasm_capabilities_keep_hot_math_inside_the_module() {
    assert_eq!(Target::parse("wasm").unwrap(), Target::Wasm);
    assert!(Target::Wasm.has("native_float").unwrap());
    assert!(!Target::Wasm.has("host_math").unwrap());
    assert_eq!(Target::Wasm.layout(), Target::Native.layout());
    let mut module = Module::new("browser".into());
    module.target = Target::Wasm;
    assert_eq!(
        rcc_ir::parse_module(&rcc_ir::emit_module(&module)).unwrap(),
        module
    );
    assert_eq!(
        serde_json::from_str::<Module>(&serde_json::to_string(&module).unwrap()).unwrap(),
        module
    );
}
