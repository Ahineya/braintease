//! Reachability follows exported functions, globals, and nested initializers.
use rcc_ir::{ir::*, link::link_modules, target::Target};

fn parse(text: &str) -> Module {
    rcc_ir::parse_module(text).unwrap()
}
#[test]
fn exported_library_roots_keep_private_callbacks_and_drop_unused_data() {
    let a = parse(
        r#"module a {
 target wasm
 internal function @callback
 @table = private [1 x i16*] = {func @callback}
 @unused = external [64000 x i16]
 function i16 @callback() { block 0: ret 17 }
 function i16* @init() { block 0: ret global @table }
 function i16 @dead() { block 0: ret global @unused }
}"#,
    );
    let b = parse(
        r#"module b {
 target wasm
 internal function @callback
 @table = private [1 x i16*] = {fat(func @callback, unknown)}
 function i16 @callback() { block 0: ret 29 }
 function i16* @step() { block 0: ret global @table }
}"#,
    );
    let linked = link_modules(&[a, b], Target::Wasm, &["init", "step"]).unwrap();
    assert_eq!(
        linked
            .functions
            .iter()
            .map(|f| f.name.as_str())
            .collect::<Vec<_>>(),
        vec!["__unit0_callback", "__unit1_callback", "init", "step"]
    );
    assert_eq!(
        linked
            .globals
            .iter()
            .map(|g| g.name.as_str())
            .collect::<Vec<_>>(),
        vec!["__unit0_table", "__unit1_table"]
    );
    assert_eq!(
        linked.internal_functions,
        vec!["__unit0_callback", "__unit1_callback"]
    );
    assert!(
        matches!(&linked.globals[0].initializer, Some(Value::ConstantAggregate(v)) if matches!(&v[0], Value::Function(n) if n == "__unit0_callback"))
    );
    assert!(
        matches!(&linked.globals[1].initializer, Some(Value::ConstantAggregate(v)) if matches!(&v[0], Value::FatPtr(p) if matches!(p.addr.as_ref(), Value::Function(n) if n == "__unit1_callback")))
    );
}

#[test]
fn linker_rejects_mixed_targets_and_duplicate_definitions_before_dce() {
    let mut a = parse("module a { target wasm function i16 @main() { block 0: ret 0 } }");
    assert!(
        link_modules(&[a.clone(), a.clone()], Target::Wasm, &["main"])
            .unwrap_err()
            .contains("multiple definitions")
    );
    a.target = Target::Native;
    assert!(link_modules(&[a], Target::Wasm, &["main"])
        .unwrap_err()
        .contains("not compiled for wasm"));
}
