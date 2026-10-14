use super::*;
use rcc_ir::ir::{BasicBlock, FatPointer};

fn pointer_local() -> Function {
    let pointer = IrType::FatPtr(Box::new(IrType::I32));
    let mut function = Function::new("fill".into(), pointer.clone());
    function.add_parameter(0, pointer.clone());
    let mut block = BasicBlock::new(0);
    block.instructions = vec![
        Instruction::Alloca {
            result: 1,
            alloc_type: pointer.clone(),
            count: None,
            result_type: IrType::FatPtr(Box::new(pointer.clone())),
        },
        Instruction::Store {
            ptr: Value::Temp(1), value: Value::Temp(0),
            value_type: pointer.clone(), volatile: false,
        },
        Instruction::Load {
            result: 2, ptr: Value::Temp(1), result_type: pointer.clone(), volatile: false,
        },
        Instruction::GetElementPtr {
            result: 3,
            ptr: Value::FatPtr(FatPointer { addr: Box::new(Value::Temp(2)), bank: BankTag::Mixed }),
            indices: vec![Value::Constant(1)],
            result_type: pointer.clone(), source_element_type: IrType::I32, is_struct_field: false,
        },
        Instruction::Store {
            ptr: Value::Temp(3), value: Value::Constant(0x12345678),
            value_type: IrType::I32, volatile: false,
        },
        Instruction::Load {
            result: 4, ptr: Value::Temp(1), result_type: pointer, volatile: false,
        },
        Instruction::Return(Some(Value::Temp(4))),
    ];
    function.blocks.push(block);
    function
}

#[test]
fn single_store_pointer_parameter_reuses_the_original_address_and_bank() {
    let mut function = pointer_local();
    promote_single_store_locals(&mut function);
    let code = &function.blocks[0].instructions;
    assert_eq!(code.len(), 3);
    assert!(matches!(&code[0], Instruction::GetElementPtr { ptr: Value::FatPtr(p), .. }
        if *p.addr == Value::Temp(0) && p.bank == BankTag::Mixed));
    assert!(matches!(code[1], Instruction::Store { value_type: IrType::I32, .. }));
    assert_eq!(code[2], Instruction::Return(Some(Value::Temp(0))));
}

#[test]
fn pointer_sources_from_gep_are_typed_and_keep_their_bank_provenance() {
    let mut function = pointer_local();
    let mut gep = function.blocks[0].instructions[3].clone();
    if let Instruction::GetElementPtr { result, ptr, .. } = &mut gep {
        *result = 8;
        *ptr = Value::Temp(0);
    }
    function.blocks[0].instructions.insert(0, gep);
    if let Instruction::Store { value, .. } = &mut function.blocks[0].instructions[2] {
        *value = Value::FatPtr(FatPointer { addr: Box::new(Value::Temp(8)), bank: BankTag::Mixed });
    }
    promote_single_store_locals(&mut function);
    assert_eq!(function.blocks[0].instructions.last(), Some(&Instruction::Return(Some(Value::Temp(8)))));
    assert!(!function.blocks[0].instructions.iter().any(|i| matches!(i,
        Instruction::Alloca { .. } | Instruction::Load { .. })));
}

#[test]
fn volatile_and_escaped_pointer_locals_remain_in_memory() {
    for escaped in [false, true] {
        let mut function = pointer_local();
        if escaped {
            function.blocks[0].instructions.insert(2, Instruction::Call {
                result: None, function: Value::Function("mutate".into()),
                args: vec![Value::Temp(1)], result_type: IrType::Void, stack_args_from: None,
            });
        } else if let Instruction::Load { volatile, .. } = &mut function.blocks[0].instructions[2] {
            *volatile = true;
        }
        let original = function.blocks.clone();
        promote_single_store_locals(&mut function);
        assert_eq!(function.blocks, original);
    }
}

#[test]
fn explicit_pointer_values_are_not_replaced_with_untyped_offsets() {
    let mut function = pointer_local();
    if let Instruction::Store { value, .. } = &mut function.blocks[0].instructions[1] {
        *value = Value::FatPtr(FatPointer { addr: Box::new(Value::Constant(7)), bank: BankTag::Heap(3) });
    }
    let original = function.blocks.clone();
    promote_single_store_locals(&mut function);
    assert_eq!(function.blocks, original);
}
