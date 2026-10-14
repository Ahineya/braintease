//! Tests for branch instruction lowering

use rcc_ir::ir::Value;
use crate::regmgmt::RegisterPressureManager;
use crate::naming::NameGenerator;
use crate::instr::branch::{lower_branch, lower_branch_cond, lower_compare_and_branch, ComparisonType};
use rcc_codegen::{AsmInst, Reg};
fn create_test_manager() -> RegisterPressureManager {
    let mut mgr = RegisterPressureManager::new(10); // 10 local variables
    mgr.init(); // Initialize with SB = 1
    mgr
}

#[test]
fn test_unconditional_branch_simple() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    // Test simple unconditional branch
    let insts = lower_branch(&mut mgr, &mut naming, 42);
    
    // Should generate unconditional branch (BEQ R0, R0, label) and comment
    assert!(insts.len() >= 1);
    
    // First instruction should be unconditional branch
    assert!(matches!(insts[0], AsmInst::Beq(Reg::R0, Reg::R0, _)));
    
    // Should have a comment explaining the jump
    let has_comment = insts.iter().any(|inst| matches!(inst, AsmInst::Comment(_)));
    assert!(has_comment, "Should have explanatory comment");
}

#[test]
fn test_conditional_branch_with_zero() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    // Test branching on constant 0 (always false)
    let insts = lower_branch_cond(
        &mut mgr,
        &mut naming,
        &Value::Constant(0),
        100, // true_label (should not be taken)
        200, // false_label (should be taken)
    );
    
    // Should load 0 and branch
    assert!(insts.len() >= 2);
    
    // Should have BEQ comparing with R0 (or possibly LI and then BEQ)
    let has_beq = insts.iter().any(|inst| matches!(inst, AsmInst::Beq(_, _, _)));
    assert!(has_beq, "Should have BEQ instruction, got: {:?}", insts);
}

#[test]
fn test_conditional_branch_with_nonzero() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    // Test branching on non-zero constant (always true)
    let insts = lower_branch_cond(
        &mut mgr,
        &mut naming,
        &Value::Constant(1),
        100, // true_label (should be taken)
        200, // false_label (should not be taken)
    );
    
    // Should load 1 and branch
    assert!(insts.len() >= 2);
    
    // Should have BEQ comparing with R0 (branches if equal to 0, i.e., false)
    let has_beq = insts.iter().any(|inst| matches!(inst, AsmInst::Beq(_, _, _)));
    assert!(has_beq, "Should have BEQ instruction");
}

#[test]
fn test_conditional_branch_with_temp() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    // Pre-allocate a register for the temp
    let _reg = mgr.get_register("t10".to_string());
    
    // Test branching on temp value
    let insts = lower_branch_cond(
        &mut mgr,
        &mut naming,
        &Value::Temp(10),
        100, // true_label
        200, // false_label
    );
    
    // Should use the temp's register directly
    assert!(insts.len() >= 2);
    
    // Should have BEQ instruction (could be after register loads)
    let has_beq = insts.iter().any(|inst| matches!(inst, AsmInst::Beq(_, _, _)));
    assert!(has_beq, "Should have BEQ instruction, got: {:?}", insts);
}

#[test]
fn test_compare_branch_equality() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    // Pre-allocate registers for temps
    let _r1 = mgr.get_register("t1".to_string());
    let _r2 = mgr.get_register("t2".to_string());
    
    // Test equality comparison
    let insts = lower_compare_and_branch(
        &mut mgr,
        &mut naming,
        &Value::Temp(1),
        &Value::Temp(2),
        ComparisonType::Eq,
        100, // true_label (if t1 == t2)
        200, // false_label (if t1 != t2)
    );
    
    // Should have BEQ to true_label (or possibly a spill instruction first)
    // Find the first branch instruction
    let beq_idx = insts.iter().position(|i| matches!(i, AsmInst::Beq(_, _, _)));
    assert!(beq_idx.is_some(), "Should have BEQ instruction, got: {:?}", insts);
    
    // Should have unconditional branch to false_label
    let has_uncond = insts.iter().any(|inst| matches!(inst, AsmInst::Beq(Reg::R0, Reg::R0, _)));
    assert!(has_uncond, "Should have unconditional branch to false_label");
}

#[test]
fn test_compare_branch_not_equal() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    // Pre-allocate registers for temps
    let _r1 = mgr.get_register("t1".to_string());
    let _r2 = mgr.get_register("t2".to_string());
    
    // Test inequality comparison
    let insts = lower_compare_and_branch(
        &mut mgr,
        &mut naming,
        &Value::Temp(1),
        &Value::Temp(2),
        ComparisonType::Ne,
        100, // true_label (if t1 != t2)
        200, // false_label (if t1 == t2)
    );
    
    // Should have BNE to true_label (or possibly a spill instruction first)
    let bne_idx = insts.iter().position(|i| matches!(i, AsmInst::Bne(_, _, _)));
    assert!(bne_idx.is_some(), "Should have BNE instruction, got: {:?}", insts);
    
    // Should have unconditional branch to false_label
    let has_uncond = insts.iter().any(|inst| matches!(inst, AsmInst::Beq(Reg::R0, Reg::R0, _)));
    assert!(has_uncond, "Should have unconditional branch to false_label");
}

#[test]
fn test_compare_branch_less_than() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    // Pre-allocate registers for temps
    let _r1 = mgr.get_register("t1".to_string());
    let _r2 = mgr.get_register("t2".to_string());
    
    // Test less than comparison
    let insts = lower_compare_and_branch(
        &mut mgr,
        &mut naming,
        &Value::Temp(1),
        &Value::Temp(2),
        ComparisonType::Lt,
        100, // true_label (if t1 < t2)
        200, // false_label (if t1 >= t2)
    );
    
    // Should have BLT to true_label (or possibly a spill instruction first)
    let blt_idx = insts.iter().position(|i| matches!(i, AsmInst::Blt(_, _, _)));
    assert!(blt_idx.is_some(), "Should have BLT instruction, got: {:?}", insts);
    
    // Should have unconditional branch to false_label
    let has_uncond = insts.iter().any(|inst| matches!(inst, AsmInst::Beq(Reg::R0, Reg::R0, _)));
    assert!(has_uncond, "Should have unconditional branch to false_label");
}

#[test]
fn test_compare_branch_greater_equal() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    // Pre-allocate registers for temps
    let _r1 = mgr.get_register("t1".to_string());
    let _r2 = mgr.get_register("t2".to_string());
    
    // Test greater than or equal comparison
    let insts = lower_compare_and_branch(
        &mut mgr,
        &mut naming,
        &Value::Temp(1),
        &Value::Temp(2),
        ComparisonType::Ge,
        100, // true_label (if t1 >= t2)
        200, // false_label (if t1 < t2)
    );
    
    // Should have BGE to true_label (or possibly a spill instruction first)
    let bge_idx = insts.iter().position(|i| matches!(i, AsmInst::Bge(_, _, _)));
    assert!(bge_idx.is_some(), "Should have BGE instruction, got: {:?}", insts);
    
    // Should have unconditional branch to false_label
    let has_uncond = insts.iter().any(|inst| matches!(inst, AsmInst::Beq(Reg::R0, Reg::R0, _)));
    assert!(has_uncond, "Should have unconditional branch to false_label");
}

#[test]
fn test_compare_branch_greater_than() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    // Pre-allocate registers for temps
    let _r1 = mgr.get_register("t1".to_string());
    let _r2 = mgr.get_register("t2".to_string());
    
    // Test greater than comparison (uses inverse logic)
    let insts = lower_compare_and_branch(
        &mut mgr,
        &mut naming,
        &Value::Temp(1),
        &Value::Temp(2),
        ComparisonType::Gt,
        100, // true_label (if t1 > t2)
        200, // false_label (if t1 <= t2)
    );
    
    // For GT, we use BGE with swapped operands to branch to false_label
    // This tests if t2 >= t1, which means t1 <= t2 (the inverse of what we want)
    let bge_idx = insts.iter().position(|i| matches!(i, AsmInst::Bge(_, _, _)));
    assert!(bge_idx.is_some(), "Should have BGE instruction, got: {:?}", insts);
    
    // Should have unconditional branch to true_label
    let has_uncond = insts.iter().any(|inst| matches!(inst, AsmInst::Beq(Reg::R0, Reg::R0, _)));
    assert!(has_uncond, "Should have unconditional branch to true_label");
}

#[test]
fn test_compare_branch_less_equal() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    // Pre-allocate registers for temps
    let _r1 = mgr.get_register("t1".to_string());
    let _r2 = mgr.get_register("t2".to_string());
    
    // Test less than or equal comparison (uses inverse logic)
    let insts = lower_compare_and_branch(
        &mut mgr,
        &mut naming,
        &Value::Temp(1),
        &Value::Temp(2),
        ComparisonType::Le,
        100, // true_label (if t1 <= t2)
        200, // false_label (if t1 > t2)
    );
    
    // For LE, we use BLT with swapped operands to branch to false_label
    // This tests if t2 < t1, which means t1 > t2 (the inverse of what we want)
    let blt_idx = insts.iter().position(|i| matches!(i, AsmInst::Blt(_, _, _)));
    assert!(blt_idx.is_some(), "Should have BLT instruction, got: {:?}", insts);
    
    // Should have unconditional branch to true_label
    let has_uncond = insts.iter().any(|inst| matches!(inst, AsmInst::Beq(Reg::R0, Reg::R0, _)));
    assert!(has_uncond, "Should have unconditional branch to true_label");
}

#[test]
fn test_branch_with_constants() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    // Test comparing two constants
    let insts = lower_compare_and_branch(
        &mut mgr,
        &mut naming,
        &Value::Constant(10),
        &Value::Constant(20),
        ComparisonType::Lt,
        100, // true_label (10 < 20, should be taken)
        200, // false_label
    );
    
    // Should load both constants and compare
    assert!(insts.len() >= 3); // At least 2 loads and a branch
    
    // Should have BLT instruction
    let has_blt = insts.iter().any(|inst| matches!(inst, AsmInst::Blt(_, _, _)));
    assert!(has_blt, "Should have BLT instruction");
}

#[test]
fn test_branch_register_spilling() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    // Fill up registers to force spilling
    for i in 0..12 {
        let _reg = mgr.get_register(format!("var{}", i));
    }
    
    // Now try to branch with a new value (should cause spilling)
    let insts = lower_branch_cond(
        &mut mgr,
        &mut naming,
        &Value::Constant(42),
        100,
        200,
    );
    
    // Should have spill/reload instructions
    assert!(insts.len() > 2, "Should have additional instructions for spilling");
    
    // Check that we got the expected branch instruction
    let has_beq = insts.iter().any(|inst| matches!(inst, AsmInst::Beq(_, _, _)));
    assert!(has_beq, "Should have BEQ instruction even with spilling");
}

// Additional tests moved from branch.rs internal tests

#[test]
fn test_unconditional_branch() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    let insts = lower_branch(&mut mgr, &mut naming, 10);
    
    // Should generate an unconditional branch (BEQ R0, R0, label)
    assert!(insts.len() >= 1);
    assert!(matches!(insts[0], AsmInst::Beq(Reg::R0, Reg::R0, _)));
}

#[test]
fn test_compare_and_branch_eq_simple() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    // Pre-allocate registers for temps
    let _r1 = mgr.get_register("t1".to_string());
    let _r2 = mgr.get_register("t2".to_string());
    
    let insts = lower_compare_and_branch(
        &mut mgr,
        &mut naming,
        &Value::Temp(1),
        &Value::Temp(2),
        ComparisonType::Eq,
        10, // true_label
        20, // false_label
    );
    
    // Should generate BEQ to true_label (or possibly loads first)
    assert!(insts.len() >= 2);
    let has_beq = insts.iter().any(|inst| matches!(inst, AsmInst::Beq(_, _, _)));
    assert!(has_beq, "Should have BEQ instruction");
}

#[test]
fn test_compare_and_branch_lt_simple() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    // Pre-allocate registers for temps
    let _r1 = mgr.get_register("t1".to_string());
    let _r2 = mgr.get_register("t2".to_string());
    
    let insts = lower_compare_and_branch(
        &mut mgr,
        &mut naming,
        &Value::Temp(1),
        &Value::Temp(2),
        ComparisonType::Lt,
        10, // true_label
        20, // false_label
    );
    
    // Should generate BLT to true_label (or possibly loads first)
    assert!(insts.len() >= 2);
    let has_blt = insts.iter().any(|inst| matches!(inst, AsmInst::Blt(_, _, _)));
    assert!(has_blt, "Should have BLT instruction");
}

#[test]
fn test_compare_and_branch_gt_simple() {
    let mut mgr = create_test_manager();
    let mut naming = NameGenerator::new(0);
    
    // Pre-allocate registers for temps
    let _r1 = mgr.get_register("t1".to_string());
    let _r2 = mgr.get_register("t2".to_string());
    
    let insts = lower_compare_and_branch(
        &mut mgr,
        &mut naming,
        &Value::Temp(1),
        &Value::Temp(2),
        ComparisonType::Gt,
        10, // true_label
        20, // false_label
    );
    
    // For GT, we use inverse logic: BGE with swapped operands to false_label
    assert!(insts.len() >= 2);
    let has_bge = insts.iter().any(|inst| matches!(inst, AsmInst::Bge(_, _, _)));
    assert!(has_bge, "Should have BGE instruction");
}