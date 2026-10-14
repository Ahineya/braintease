//! Tests for Binary operation lowering

use rcc_ir::ir::{Value, IrBinaryOp};
use crate::regmgmt::RegisterPressureManager;
use crate::naming::new_function_naming;
use crate::instr::{lower_binary_op, lower_binary_op_immediate};
use rcc_codegen::AsmInst;

#[test]
fn test_add_operation() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let lhs = Value::Temp(1);
    let rhs = Value::Temp(2);
    let result_temp = 3;
    
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Add, 
                                &lhs, &rhs, result_temp);
    
    // Should generate ADD instruction
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Add(_, _, _))));
}

#[test]
fn test_immediate_optimization() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let lhs = Value::Temp(1);
    let rhs_const = 42;
    let result_temp = 2;
    
    let insts = lower_binary_op_immediate(&mut mgr, &mut naming,
                                          IrBinaryOp::Add, &lhs, 
                                          rhs_const, result_temp);
    
    // Should generate ADDI instruction
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::AddI(_, _, 42))));
}

#[test]
fn test_logical_immediate_fallback() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let lhs = Value::Temp(1);
    let rhs_const = 0xFF;
    let result_temp = 2;
    
    // Test AND with immediate - should load constant and use regular AND
    let insts = lower_binary_op_immediate(&mut mgr, &mut naming,
                                          IrBinaryOp::And, &lhs, 
                                          rhs_const, result_temp);
    
    // Should generate LI followed by AND
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Li(_, 0xFF))));
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::And(_, _, _))));
}

#[test]
fn test_comparison_operations() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let lhs = Value::Temp(1);
    let rhs = Value::Temp(2);
    
    // Test EQ - should generate XOR and SLTU
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Eq, 
                                &lhs, &rhs, 3);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Xor(_, _, _))));
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Sltu(_, _, _))));
    
    // Reset for next test
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Test SLE - should generate SLT and SUB
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Sle, 
                                &lhs, &rhs, 4);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Slt(_, _, _))));
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Sub(_, _, _))));
}

#[test]
fn test_commutative_operations() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Test that commutative operations can be evaluated in either order
    let lhs = Value::Temp(1);
    let rhs = Value::Temp(2);
    
    // Test MUL - commutative operation
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Mul,
                                &lhs, &rhs, 3);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Mul(_, _, _))));
    
    // Test XOR - commutative operation
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Xor,
                                &lhs, &rhs, 4);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Xor(_, _, _))));
}

#[test]
fn test_non_commutative_operations() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let lhs = Value::Temp(1);
    let rhs = Value::Temp(2);
    
    // Test SUB - non-commutative operation
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Sub,
                                &lhs, &rhs, 3);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Sub(_, _, _))));
    
    // Test SHL - non-commutative operation
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Shl,
                                &lhs, &rhs, 4);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Sll(_, _, _))));
}

#[test]
fn test_division_operations() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let lhs = Value::Temp(1);
    let rhs = Value::Temp(2);
    
    // Test unsigned division
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::UDiv,
                                &lhs, &rhs, 3);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Div(_, _, _))));
    
    // Test unsigned remainder
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::URem,
                                &lhs, &rhs, 4);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Mod(_, _, _))));
}

#[test]
fn test_immediate_division() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let lhs = Value::Temp(1);
    
    // Test division by immediate
    let insts = lower_binary_op_immediate(&mut mgr, &mut naming,
                                          IrBinaryOp::UDiv, &lhs, 
                                          4, 2);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::DivI(_, _, 4))));
    
    // Test modulo by immediate
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let insts = lower_binary_op_immediate(&mut mgr, &mut naming,
                                          IrBinaryOp::URem, &lhs, 
                                          8, 3);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::ModI(_, _, 8))));
}

#[test]
fn test_shift_operations() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let lhs = Value::Temp(1);
    let rhs = Value::Temp(2);
    
    // Test left shift
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Shl,
                                &lhs, &rhs, 3);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Sll(_, _, _))));
    
    // Test logical right shift
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::LShr,
                                &lhs, &rhs, 4);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Srl(_, _, _))));
}

#[test]
fn test_unsigned_comparisons() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let lhs = Value::Temp(1);
    let rhs = Value::Temp(2);
    
    // Test ULT
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Ult,
                                &lhs, &rhs, 3);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Sltu(_, _, _))));
    
    // Test UGT - should generate SLTU with swapped operands
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Ugt,
                                &lhs, &rhs, 4);
    // UGT is implemented as SLTU(rhs, lhs)
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Sltu(_, _, _))));
}

#[test]
fn test_ne_operation() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let lhs = Value::Temp(1);
    let rhs = Value::Temp(2);
    
    // Test NE - should generate XOR and SLTU to check if result is non-zero
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Ne,
                                &lhs, &rhs, 3);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Xor(_, _, _))));
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Sltu(_, _, _))));
}

#[test]
fn test_subtract_immediate_as_add_negative() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let lhs = Value::Temp(1);
    let rhs_const = 10;
    let result_temp = 2;
    
    // Test SUB with immediate - should generate ADDI with negated value
    let insts = lower_binary_op_immediate(&mut mgr, &mut naming,
                                          IrBinaryOp::Sub, &lhs, 
                                          rhs_const, result_temp);
    
    // Should generate ADDI with -10
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::AddI(_, _, -10))));
}

#[test]
fn test_constant_operands() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Test with constant operands
    let lhs = Value::Constant(100);
    let rhs = Value::Constant(200);
    
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Add,
                                &lhs, &rhs, 1);
    
    // Should load constants and then add
    // The constants should be loaded into registers before the ADD
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Add(_, _, _))));
}

#[test]
fn test_sgt_operation() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let lhs = Value::Temp(1);
    let rhs = Value::Temp(2);
    
    // Test SGT - implemented as SLT(rhs, lhs)
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Sgt,
                                &lhs, &rhs, 3);
    
    // Should generate SLT with swapped operands
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Slt(_, _, _))));
}

#[test]
fn test_sge_operation() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let lhs = Value::Temp(1);
    let rhs = Value::Temp(2);
    
    // Test SGE - implemented as !(a < b)
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Sge,
                                &lhs, &rhs, 3);
    
    // Should generate SLT followed by subtraction from 1
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Slt(_, _, _))));
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Li(_, 1))));
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Sub(_, _, _))));
}

#[test]
fn test_ule_operation() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let lhs = Value::Temp(1);
    let rhs = Value::Temp(2);
    
    // Test ULE - implemented as !(b < a)
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Ule,
                                &lhs, &rhs, 3);
    
    // Should generate SLTU with swapped operands, then subtract from 1
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Sltu(_, _, _))));
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Li(_, 1))));
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Sub(_, _, _))));
}

#[test]
fn test_uge_operation() {
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let lhs = Value::Temp(1);
    let rhs = Value::Temp(2);
    
    // Test UGE - implemented as !(a < b)
    let insts = lower_binary_op(&mut mgr, &mut naming, IrBinaryOp::Uge,
                                &lhs, &rhs, 3);
    
    // Should generate SLTU, then subtract from 1
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Sltu(_, _, _))));
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Li(_, 1))));
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Sub(_, _, _))));
}