//! Branch instruction lowering for V2 backend
//! 
//! Implements conditional and unconditional branch instructions.
//! Note: Branches in the VM use relative addresses, while JAL uses absolute addresses.

use rcc_ir::ir::Value;
use rcc_common::LabelId;
use crate::regmgmt::RegisterPressureManager;
use crate::naming::NameGenerator;
use crate::instr::helpers::get_value_register;
use rcc_codegen::{AsmInst, Reg};
use log::{debug, trace};

/// Lower an unconditional branch to assembly instructions
/// 
/// # Parameters
/// - `mgr`: Register pressure manager for allocation and spilling
/// - `naming`: Name generator for unique temporary names
/// - `target_label`: Label to branch to
/// 
/// # Returns
/// Vector of assembly instructions for the unconditional branch
pub fn lower_branch(
    _mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    target_label: LabelId,
) -> Vec<AsmInst> {
    debug!("lower_branch: target=L{target_label}");
    
    let mut insts = vec![];
    
    // For unconditional branches, we need to generate a jump to the label
    // The V2 backend will need to handle this at a higher level by tracking
    // label positions and generating appropriate JAL instructions.
    // For now, we'll emit a placeholder that the higher-level code must handle.
    let label_name = naming.label_name(target_label);
    
    // We'll use BEQ with two equal registers to create an unconditional branch
    // BEQ R0, R0, label will always branch since R0 == R0
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, label_name.clone()));
    insts.push(AsmInst::Comment(format!("Unconditional branch to {label_name}")));
    
    trace!("  Generated unconditional branch to {label_name}");
    debug!("lower_branch complete: generated {} instructions", insts.len());
    
    insts
}

/// Lower a conditional branch to assembly instructions
/// 
/// # Parameters
/// - `mgr`: Register pressure manager for allocation and spilling
/// - `naming`: Name generator for unique temporary names
/// - `condition`: Value that determines the branch (0 = false, non-zero = true)
/// - `true_label`: Label to branch to if condition is true
/// - `false_label`: Label to branch to if condition is false
/// 
/// # Returns
/// Vector of assembly instructions for the conditional branch
pub fn lower_branch_cond(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    condition: &Value,
    true_label: LabelId,
    false_label: LabelId,
) -> Vec<AsmInst> {
    debug!("lower_branch_cond: condition={condition:?}, true=L{true_label}, false=L{false_label}");
    trace!("  Current register state: spill_count={}", mgr.get_spill_count());
    
    let mut insts = vec![];
    
    // Get register for condition value
    let cond_reg = get_value_register(mgr, naming, condition);
    insts.extend(mgr.take_instructions());
    trace!("  Condition in {cond_reg:?}");
    
    // Generate branch instructions
    // BEQ/BNE use relative addresses (offset from current PC)
    // The assembler will calculate the correct relative offset
    
    let true_label_name = naming.label_name(true_label);
    let false_label_name = naming.label_name(false_label);
    
    // Branch if condition is zero (false) to false_label
    // Otherwise fall through to branch to true_label
    insts.push(AsmInst::Beq(cond_reg, Reg::R0, false_label_name.clone()));
    insts.push(AsmInst::Comment(format!("Branch to {false_label_name} if condition is false")));
    
    // If condition was non-zero (true), branch to true_label
    // Use unconditional branch (BEQ with R0 == R0)
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, true_label_name.clone()));
    insts.push(AsmInst::Comment(format!("Unconditional branch to {true_label_name} (condition was true)")));
    
    // The false branch target is handled by the BEQ above
    
    // Free the condition register
    mgr.free_register(cond_reg);
    trace!("  Freed condition register {cond_reg:?}");
    
    debug!("lower_branch_cond complete: generated {} instructions", insts.len());
    trace!("  Final register state: spill_count={}", mgr.get_spill_count());
    
    insts
}

/// Lower a comparison-based branch (common pattern)
/// 
/// This is a helper for the common pattern of comparing two values and branching.
/// 
/// # Parameters
/// - `mgr`: Register pressure manager for allocation and spilling
/// - `naming`: Name generator for unique temporary names
/// - `lhs`: Left-hand side value to compare
/// - `rhs`: Right-hand side value to compare
/// - `comparison`: Type of comparison (eq, ne, lt, ge)
/// - `true_label`: Label to branch to if comparison is true
/// - `false_label`: Label to branch to if comparison is false
/// 
/// # Returns
/// Vector of assembly instructions for the comparison and branch
pub fn lower_compare_and_branch(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    lhs: &Value,
    rhs: &Value,
    comparison: ComparisonType,
    true_label: LabelId,
    false_label: LabelId,
) -> Vec<AsmInst> {
    debug!("lower_compare_and_branch: lhs={lhs:?}, rhs={rhs:?}, cmp={comparison:?}, true=L{true_label}, false=L{false_label}");
    
    let mut insts = vec![];
    
    // Get registers for operands
    let lhs_reg = get_value_register(mgr, naming, lhs);
    insts.extend(mgr.take_instructions());
    let rhs_reg = get_value_register(mgr, naming, rhs);
    insts.extend(mgr.take_instructions());
    
    trace!("  LHS in {lhs_reg:?}, RHS in {rhs_reg:?}");
    
    let true_label_name = naming.label_name(true_label);
    let false_label_name = naming.label_name(false_label);
    
    // Generate the appropriate branch instruction based on comparison type
    match comparison {
        ComparisonType::Eq => {
            // Branch to true_label if lhs == rhs
            insts.push(AsmInst::Beq(lhs_reg, rhs_reg, true_label_name.clone()));
            insts.push(AsmInst::Comment(format!("Branch to {true_label_name} if {lhs_reg} == {rhs_reg}")));
        }
        ComparisonType::Ne => {
            // Branch to true_label if lhs != rhs
            insts.push(AsmInst::Bne(lhs_reg, rhs_reg, true_label_name.clone()));
            insts.push(AsmInst::Comment(format!("Branch to {true_label_name} if {lhs_reg} != {rhs_reg}")));
        }
        ComparisonType::Lt => {
            // Branch to true_label if lhs < rhs
            insts.push(AsmInst::Blt(lhs_reg, rhs_reg, true_label_name.clone()));
            insts.push(AsmInst::Comment(format!("Branch to {true_label_name} if {lhs_reg} < {rhs_reg}")));
        }
        ComparisonType::Ge => {
            // Branch to true_label if lhs >= rhs
            insts.push(AsmInst::Bge(lhs_reg, rhs_reg, true_label_name.clone()));
            insts.push(AsmInst::Comment(format!("Branch to {true_label_name} if {lhs_reg} >= {rhs_reg}")));
        }
        ComparisonType::Le => {
            // Branch to false_label if lhs > rhs (inverse of <=)
            // This is equivalent to: if !(lhs <= rhs) goto false; else goto true;
            // Which is: if (lhs > rhs) goto false; else goto true;
            // We can use Blt with swapped operands: if (rhs < lhs) goto false
            insts.push(AsmInst::Blt(rhs_reg, lhs_reg, false_label_name.clone()));
            insts.push(AsmInst::Comment(format!("Branch to {false_label_name} if {lhs_reg} > {rhs_reg} (inverse of <=)")));
            // Fall through to branch to true_label
            insts.push(AsmInst::Beq(Reg::R0, Reg::R0, true_label_name.clone()));
            insts.push(AsmInst::Comment(format!("Unconditional branch to {true_label_name} ({lhs_reg} <= {rhs_reg})")));
            
            // Free registers and return early since we handled both branches
            mgr.free_register(lhs_reg);
            mgr.free_register(rhs_reg);
            return insts;
        }
        ComparisonType::Gt => {
            // Branch to false_label if lhs <= rhs (inverse of >)
            // We can use Bge with swapped operands: if (rhs >= lhs) goto false
            insts.push(AsmInst::Bge(rhs_reg, lhs_reg, false_label_name.clone()));
            insts.push(AsmInst::Comment(format!("Branch to {false_label_name} if {lhs_reg} <= {rhs_reg} (inverse of >)")));
            // Fall through to branch to true_label
            insts.push(AsmInst::Beq(Reg::R0, Reg::R0, true_label_name.clone()));
            insts.push(AsmInst::Comment(format!("Unconditional branch to {true_label_name} ({lhs_reg} > {rhs_reg})")));
            
            // Free registers and return early since we handled both branches
            mgr.free_register(lhs_reg);
            mgr.free_register(rhs_reg);
            return insts;
        }
    }
    
    // For eq, ne, lt, ge: if we didn't branch to true_label, branch to false_label
    insts.push(AsmInst::Beq(Reg::R0, Reg::R0, false_label_name.clone()));
    insts.push(AsmInst::Comment(format!("Unconditional branch to {false_label_name} (condition was false)")));
    
    // Free registers
    mgr.free_register(lhs_reg);
    mgr.free_register(rhs_reg);
    
    debug!("lower_compare_and_branch complete: generated {} instructions", insts.len());
    
    insts
}

/// Comparison types for branch instructions
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ComparisonType {
    Eq,  // Equal
    Ne,  // Not equal
    Lt,  // Less than (signed)
    Le,  // Less than or equal (signed)
    Gt,  // Greater than (signed)
    Ge,  // Greater than or equal (signed)
}