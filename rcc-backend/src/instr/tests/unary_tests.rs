//! Tests for Unary operation lowering

use rcc_ir::ir::{Value, IrUnaryOp, IrType};
use crate::regmgmt::RegisterPressureManager;
use crate::naming::new_function_naming;
use crate::instr::lower_unary_op;
use rcc_codegen::AsmInst;

#[test]
fn test_not_operation() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let operand = Value::Temp(1);
    let result_temp = 2;
    
    let insts = lower_unary_op(&mut mgr, &mut naming, IrUnaryOp::Not, 
                               &operand, &IrType::I16, result_temp);
    
    // Should generate LI -1 followed by XOR
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Li(_, -1))));
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Xor(_, _, _))));
}

#[test]
fn test_neg_operation() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let operand = Value::Temp(1);
    let result_temp = 2;
    
    let insts = lower_unary_op(&mut mgr, &mut naming, IrUnaryOp::Neg,
                               &operand, &IrType::I16, result_temp);
    
    // Should generate LI 0 followed by SUB
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Li(_, 0))));
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Sub(_, _, _))));
}

#[test]
fn test_not_with_constant() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Test NOT of a constant value
    let operand = Value::Constant(42);
    let result_temp = 1;
    
    let insts = lower_unary_op(&mut mgr, &mut naming, IrUnaryOp::Not,
                               &operand, &IrType::I16, result_temp);
    
    // Should load constant, then NOT it
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Xor(_, _, _))));
}

#[test]
fn test_zext_operation() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let operand = Value::Temp(1);
    let result_temp = 2;
    
    let insts = lower_unary_op(&mut mgr, &mut naming, IrUnaryOp::ZExt,
                               &operand, &IrType::I16, result_temp);
    
    // Zero extend might be a no-op or a move (ADD with R0)
    // Check that we don't generate excessive instructions
    assert!(insts.len() <= 2); // At most a move instruction
}

#[test]
fn test_trunc_to_i8() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let operand = Value::Temp(1);
    let result_temp = 2;
    
    let insts = lower_unary_op(&mut mgr, &mut naming, IrUnaryOp::Trunc,
                               &operand, &IrType::I8, result_temp);
    
    // Should generate AND with 0xFF mask
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Li(_, 0xFF))));
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::And(_, _, _))));
}

#[test]
fn test_trunc_to_i1() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let operand = Value::Temp(1);
    let result_temp = 2;
    
    let insts = lower_unary_op(&mut mgr, &mut naming, IrUnaryOp::Trunc,
                               &operand, &IrType::I1, result_temp);
    
    // Should generate AND with 0x1 mask
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Li(_, 1))));
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::And(_, _, _))));
}

#[test]
fn test_ptr_to_int() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let operand = Value::Temp(1);
    let result_temp = 2;
    
    let insts = lower_unary_op(&mut mgr, &mut naming, IrUnaryOp::PtrToInt,
                               &operand, &IrType::I16, result_temp);
    
    // PtrToInt should be a simple move or no-op
    assert!(insts.len() <= 2); // At most a move instruction
}

#[test]
fn test_int_to_ptr() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let operand = Value::Temp(1);
    let result_temp = 2;
    
    let insts = lower_unary_op(&mut mgr, &mut naming, IrUnaryOp::IntToPtr,
                               &operand, &IrType::I16, result_temp);
    
    // IntToPtr should be a simple move or no-op
    assert!(insts.len() <= 2); // At most a move instruction
}

#[test]
fn test_sext_operation() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let operand = Value::Temp(1);
    let result_temp = 2;
    
    let insts = lower_unary_op(&mut mgr, &mut naming, IrUnaryOp::SExt,
                               &operand, &IrType::I16, result_temp);
    
    // Sign extend is currently treated as a move
    // TODO: When proper sign extension is implemented, update this test
    assert!(insts.len() <= 2); // At most a move instruction
}

#[test]
fn test_neg_with_immediate() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Test negation of a constant
    let operand = Value::Constant(100);
    let result_temp = 1;
    
    let insts = lower_unary_op(&mut mgr, &mut naming, IrUnaryOp::Neg,
                               &operand, &IrType::I16, result_temp);
    
    // Should load constant, then negate it
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Sub(_, _, _))));
}

#[test]
fn test_multiple_unary_ops() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Test a sequence of unary operations
    let operand1 = Value::Temp(1);
    
    // First operation: NOT
    let insts1 = lower_unary_op(&mut mgr, &mut naming, IrUnaryOp::Not,
                                &operand1, &IrType::I16, 2);
    assert!(!insts1.is_empty());
    
    // Second operation: NEG on the result
    let operand2 = Value::Temp(2);
    let insts2 = lower_unary_op(&mut mgr, &mut naming, IrUnaryOp::Neg,
                                &operand2, &IrType::I16, 3);
    assert!(!insts2.is_empty());
}

#[test]
fn test_trunc_to_i16() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let operand = Value::Temp(1);
    let result_temp = 2;
    
    // Truncating to same size should be a no-op or simple move
    let insts = lower_unary_op(&mut mgr, &mut naming, IrUnaryOp::Trunc,
                               &operand, &IrType::I16, result_temp);
    
    // Should be minimal instructions (just a move if needed)
    assert!(insts.len() <= 2);
}