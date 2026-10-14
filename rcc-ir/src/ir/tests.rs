//! Unit tests for the IR module

use super::*;

#[test]
fn test_ir_values() {
    let temp = Value::Temp(5);
    let constant = Value::Constant(42);
    let global = Value::Global("main".to_string());
    
    assert_eq!(format!("{}", temp), "%5");
    assert_eq!(format!("{}", constant), "42");
    assert_eq!(format!("{}", global), "@main");
}

#[test]
fn test_basic_block() {
    let mut block = BasicBlock::new(0);
    assert!(block.is_empty());
    assert!(!block.has_terminator());
    
    block.add_instruction(Instruction::Comment("test".to_string()));
    assert!(!block.is_empty());
    assert!(!block.has_terminator());
    
    block.add_instruction(Instruction::Return(Some(Value::Constant(0))));
    assert!(block.has_terminator());
}

#[test]
fn test_function() {
    let mut function = Function::new("test".to_string(), IrType::I32);
    function.add_parameter(0, IrType::I32);
    function.add_parameter(1, IrType::I32);
    
    assert_eq!(function.parameters.len(), 2);
    assert_eq!(function.return_type, IrType::I32);
}

#[test]
fn test_ir_builder() {
    let mut builder = IrBuilder::new();
    
    let func = builder.create_function("add".to_string(), IrType::I32);
    func.add_parameter(0, IrType::I32);
    func.add_parameter(1, IrType::I32);
    
    let entry_label = builder.new_label();
    builder.create_block(entry_label).unwrap();
    
    let result = builder.build_binary(
        IrBinaryOp::Add,
        Value::Temp(0),
        Value::Temp(1),
        IrType::I32,
    ).unwrap();
    
    builder.build_return(Some(Value::Temp(result))).unwrap();
    
    let function = builder.finish_function().unwrap();
    assert_eq!(function.name, "add");
    assert_eq!(function.blocks.len(), 1);
    assert!(!function.blocks[0].is_empty());
}

#[test]
fn test_module() {
    let mut module = Module::new("test".to_string());
    
    let function = Function::new("main".to_string(), IrType::I32);
    module.add_function(function);
    
    let global = GlobalVariable {
        name: "global_var".to_string(),
        var_type: IrType::I32,
        is_constant: false,
        initializer: Some(Value::Constant(42)),
        linkage: Linkage::External,
        symbol_id: None,
    };
    module.add_global(global);
    
    assert_eq!(module.functions.len(), 1);
    assert_eq!(module.globals.len(), 1);
    assert!(module.get_function("main").is_some());
    assert!(module.get_global("global_var").is_some());
}