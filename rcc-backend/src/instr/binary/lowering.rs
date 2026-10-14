//! Main binary operation lowering logic

use rcc_ir::ir::{Value, IrBinaryOp};
use rcc_common::TempId;
use crate::regmgmt::RegisterPressureManager;
use crate::naming::NameGenerator;
use rcc_codegen::AsmInst;
use log::{debug, trace};

use crate::instr::helpers::get_value_register;
use super::{
    is_commutative, can_reuse_register, calculate_register_needs,
    generate_arithmetic_instruction, generate_comparison_instructions
};
use super::comparison::is_comparison;
use super::arithmetic::generate_signed_arithmetic_instructions;

/// Lower a binary operation to assembly instructions
/// 
/// This function implements the Sethi-Ullman algorithm to evaluate
/// expressions in an order that minimizes register pressure.
/// 
/// # Sethi-Ullman Algorithm
/// The algorithm evaluates the operand that requires more registers first,
/// allowing its registers to be freed before evaluating the second operand.
/// This minimizes the peak register usage during expression evaluation.
/// 
/// # Parameters
/// - `mgr`: Register pressure manager for allocation and spilling
/// - `naming`: Name generator for unique temporary names
/// - `op`: The binary operation to perform
/// - `lhs`: Left-hand side operand
/// - `rhs`: Right-hand side operand
/// - `result_temp`: Temp ID for the result
/// 
/// # Returns
/// Vector of assembly instructions for the binary operation
pub fn lower_binary_op(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    op: IrBinaryOp,
    lhs: &Value,
    rhs: &Value,
    result_temp: TempId,
) -> Vec<AsmInst> {
    debug!("lower_binary_op: op={op:?}, lhs={lhs:?}, rhs={rhs:?}, result=t{result_temp}");
    trace!("  Current register state: spill_count={}", mgr.get_spill_count());
    
    let mut insts = vec![];
    let result_name = naming.temp_name(result_temp);
    
    // Step 1: Calculate register needs for Sethi-Ullman ordering
    let (left_need, right_need) = calculate_register_needs(mgr, lhs, rhs);
    trace!("  Register needs: left={left_need}, right={right_need}");
    
    // Step 2: Determine evaluation order
    // For commutative operations, evaluate the operand with higher register need first
    // For non-commutative operations, we must evaluate in order but can still
    // optimize register allocation
    let should_swap = right_need > left_need && is_commutative(op);
    
    if should_swap {
        trace!("  Swapping operands for better register usage (commutative op)");
    }
    
    // Step 3: Get registers for operands. Pin the first so evaluating the
    // second cannot spill and reuse it.
    let (lhs_reg, rhs_reg) = if should_swap {
        let rhs_reg = get_value_register(mgr, naming, rhs);
        insts.extend(mgr.take_instructions());
        trace!("  RHS operand in {rhs_reg:?} (evaluated first due to higher need)");
        mgr.pin_register(rhs_reg);

        let lhs_reg = get_value_register(mgr, naming, lhs);
        insts.extend(mgr.take_instructions());
        trace!("  LHS operand in {lhs_reg:?}");
        mgr.pin_register(lhs_reg);

        (lhs_reg, rhs_reg)
    } else {
        let lhs_reg = get_value_register(mgr, naming, lhs);
        insts.extend(mgr.take_instructions());
        trace!("  LHS operand in {lhs_reg:?}");
        mgr.pin_register(lhs_reg);

        let rhs_reg = get_value_register(mgr, naming, rhs);
        insts.extend(mgr.take_instructions());
        trace!("  RHS operand in {rhs_reg:?}");
        mgr.pin_register(rhs_reg);

        (lhs_reg, rhs_reg)
    };

    // Step 4: Allocate result register
    // Optimization: Try to reuse first register if possible
    let result_reg = if can_reuse_register(op) && lhs_reg != rhs_reg
        && !mgr.register_is_live_after(lhs_reg) {
        trace!("  Reusing LHS register {lhs_reg:?} for result");
        lhs_reg
    } else {
        let reg = mgr.get_register(result_name.clone());
        insts.extend(mgr.take_instructions());
        trace!("  Allocated new register {reg:?} for result");
        reg
    };
    mgr.pin_register(result_reg);
    
    // Step 5: Generate the appropriate instruction(s)
    if is_comparison(op) {
        // Comparison operations need special handling
        let comp_insts = generate_comparison_instructions(
            mgr, naming, op, lhs_reg, rhs_reg, result_reg, result_temp
        );
        insts.extend(comp_insts);
        
        // Ensure the result is properly bound to the result temp
        // This is critical so future references to this temp get the right register
        mgr.bind_value_to_register(result_name.clone(), result_reg);

        mgr.unpin_register(lhs_reg);
        mgr.unpin_register(rhs_reg);
        mgr.unpin_register(result_reg);
        
        // Free only the operand registers that aren't being used for the result
        if result_reg != lhs_reg {
            mgr.free_register(lhs_reg);
        }
        if result_reg != rhs_reg {
            mgr.free_register(rhs_reg);
        }
        
        // Do NOT free result_reg - it contains our comparison result
        
        debug!("lower_binary_op complete: generated {} instructions", insts.len());
        return insts;
    }
    
    // For arithmetic operations
    if matches!(op, IrBinaryOp::SDiv | IrBinaryOp::SRem | IrBinaryOp::AShr) {
        insts.extend(generate_signed_arithmetic_instructions(
            mgr, naming, op, result_reg, lhs_reg, rhs_reg,
        ));
    } else if let Some(inst) = generate_arithmetic_instruction(op, result_reg, lhs_reg, rhs_reg, should_swap) {
        trace!("  Generated instruction: {inst:?}");
        insts.push(inst);
    } else {
        panic!("Unexpected operation type: {op:?}");
    }
    
    // Ensure the result is properly bound to the result temp
    mgr.bind_value_to_register(result_name, result_reg);

    mgr.unpin_register(lhs_reg);
    mgr.unpin_register(rhs_reg);
    mgr.unpin_register(result_reg);
    
    // Step 6: Free registers that are no longer needed
    if result_reg != lhs_reg {
        mgr.free_register(lhs_reg);
        trace!("  Freed LHS register {lhs_reg:?}");
    }
    if result_reg != rhs_reg {
        mgr.free_register(rhs_reg);
        trace!("  Freed RHS register {rhs_reg:?}");
    }
    
    debug!("lower_binary_op complete: generated {} instructions", insts.len());
    trace!("  Final register state: spill_count={}", mgr.get_spill_count());
    
    insts
}

/// Lower a binary operation with immediate right operand
/// 
/// This is an optimization for operations like `x + 5` where the
/// right operand is a small constant that fits in an immediate field.
pub fn lower_binary_op_immediate(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    op: IrBinaryOp,
    lhs: &Value,
    rhs_const: i16,
    result_temp: TempId,
) -> Vec<AsmInst> {
    debug!("lower_binary_op_immediate: op={op:?}, lhs={lhs:?}, rhs={rhs_const}, result=t{result_temp}");

    // These operations require a sequence of instructions. Delegate before
    // allocating anything, so any spill/reload instructions remain intact.
    if is_comparison(op) || matches!(op, IrBinaryOp::SDiv | IrBinaryOp::SRem | IrBinaryOp::AShr) {
        return lower_binary_op(mgr, naming, op, lhs, &Value::Constant(rhs_const as i64), result_temp);
    }
    
    let mut insts = vec![];
    let result_name = naming.temp_name(result_temp);
    
    // Get register for left operand
    let lhs_reg = get_value_register(mgr, naming, lhs);
    insts.extend(mgr.take_instructions());
    mgr.pin_register(lhs_reg);
    
    // Allocate result register (can often reuse lhs)
    let result_reg = mgr.get_register(result_name);
    insts.extend(mgr.take_instructions());
    mgr.pin_register(result_reg);
    
    // Handle operations with immediate values
    match op {
        // Operations with immediate instruction support
        IrBinaryOp::Add => {
            insts.push(AsmInst::AddI(result_reg, lhs_reg, rhs_const));
            trace!("  Generated ADDI instruction");
        }
        IrBinaryOp::Sub => {
            // SubI doesn't exist, use AddI with negative value
            insts.push(AsmInst::AddI(result_reg, lhs_reg, rhs_const.wrapping_neg()));
            trace!("  Generated ADDI with negated immediate for SUB");
        }
        IrBinaryOp::Mul => {
            insts.push(AsmInst::MulI(result_reg, lhs_reg, rhs_const));
            trace!("  Generated MULI instruction");
        }
        IrBinaryOp::UDiv => {
            // VM DIVI is unsigned - direct mapping
            insts.push(AsmInst::DivI(result_reg, lhs_reg, rhs_const));
            trace!("  Generated DIVI instruction");
        }
        IrBinaryOp::URem => {
            // VM MODI is unsigned - direct mapping
            insts.push(AsmInst::ModI(result_reg, lhs_reg, rhs_const));
            trace!("  Generated MODI instruction");
        }
        
        // Operations without immediate forms - load constant first
        IrBinaryOp::And | IrBinaryOp::Or | IrBinaryOp::Xor => {
            debug!("  No immediate form for {op:?}, loading constant into register");
            let rhs_reg = mgr.get_register(naming.imm_value(rhs_const));
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(rhs_reg, rhs_const));
            
            match op {
                IrBinaryOp::And => insts.push(AsmInst::And(result_reg, lhs_reg, rhs_reg)),
                IrBinaryOp::Or => insts.push(AsmInst::Or(result_reg, lhs_reg, rhs_reg)),
                IrBinaryOp::Xor => insts.push(AsmInst::Xor(result_reg, lhs_reg, rhs_reg)),
                _ => unreachable!(),
            }
            
            mgr.free_register(rhs_reg);
            trace!("  Generated {op:?} with loaded immediate");
        }
        
        IrBinaryOp::Shl | IrBinaryOp::LShr => {
            debug!("  No immediate form for shift, loading constant into register");
            let rhs_reg = mgr.get_register(naming.imm_value(rhs_const));
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(rhs_reg, rhs_const));
            
            match op {
                IrBinaryOp::Shl => insts.push(AsmInst::Sll(result_reg, lhs_reg, rhs_reg)),
                IrBinaryOp::LShr => {
                    insts.push(AsmInst::Srl(result_reg, lhs_reg, rhs_reg));
                }
                _ => unreachable!(),
            }
            
            mgr.free_register(rhs_reg);
            trace!("  Generated shift with loaded immediate");
        }
        
        // Delegated above before allocating registers.
        IrBinaryOp::SDiv | IrBinaryOp::SRem | IrBinaryOp::AShr |
        IrBinaryOp::Eq | IrBinaryOp::Ne | 
        IrBinaryOp::Slt | IrBinaryOp::Sle | IrBinaryOp::Sgt | IrBinaryOp::Sge |
        IrBinaryOp::Ult | IrBinaryOp::Ule | IrBinaryOp::Ugt | IrBinaryOp::Uge => {
            unreachable!();
        }
    }

    mgr.unpin_register(lhs_reg);
    mgr.unpin_register(result_reg);
    
    // Free lhs register if not reused
    if result_reg != lhs_reg {
        mgr.free_register(lhs_reg);
    }
    
    debug!("lower_binary_op_immediate complete: generated {} instructions", insts.len());
    insts
}

#[cfg(test)]
mod shared_operand_tests {
    use super::*;
    use std::collections::BTreeSet;
    use crate::instr::{lower_unary_op, tests::machine::Machine};
    use rcc_ir::ir::{IrType, IrUnaryOp};

    #[test]
    fn arithmetic_comparison_not_and_neg_preserve_future_operand_uses() {
        let mut manager = RegisterPressureManager::new(0);
        let mut names = NameGenerator::new(0);
        manager.init();
        let a = manager.get_register("t1".into());
        let b = manager.get_register("t2".into());
        let mut code = manager.take_instructions();
        code.extend([AsmInst::Li(a, 7), AsmInst::Li(b, 11)]);
        manager.set_live_after(&BTreeSet::from([1, 2, 3]));
        code.extend(lower_binary_op(&mut manager, &mut names, IrBinaryOp::Sub,
            &Value::Temp(1), &Value::Temp(2), 3));
        manager.set_live_after(&BTreeSet::from([1, 2, 3, 4]));
        code.extend(lower_binary_op(&mut manager, &mut names, IrBinaryOp::Slt,
            &Value::Temp(1), &Value::Temp(2), 4));
        manager.set_live_after(&BTreeSet::from([1, 2, 3, 4, 5]));
        code.extend(lower_unary_op(&mut manager, &mut names, IrUnaryOp::Not,
            &Value::Temp(1), &IrType::I16, 5));
        manager.set_live_after(&BTreeSet::from([1, 2, 3, 4, 5, 6]));
        code.extend(lower_unary_op(&mut manager, &mut names, IrUnaryOp::Neg,
            &Value::Temp(2), &IrType::I16, 6));
        let outputs: Vec<_> = (1..=6).map(|id| manager.get_register(format!("t{id}"))).collect();
        code.extend(manager.take_instructions());
        let mut machine = Machine::new(64000);
        machine.run(&code);
        let actual: Vec<_> = outputs.iter().map(|r| machine.regs[*r as usize]).collect();
        assert_eq!(actual, vec![7, 11, (-4i16) as u16, 1, !7u16, (-11i16) as u16]);
    }

    #[test]
    fn truncating_shared_wide_operand_does_not_destroy_low_word_or_companion() {
        let mut manager = RegisterPressureManager::new(0);
        let mut names = NameGenerator::new(0);
        manager.init();
        let low = manager.get_register("t1".into());
        let high = manager.get_register("source_hi".into());
        manager.set_i32_high("t1".into(), "source_hi".into());
        let mut code = manager.take_instructions();
        code.extend([AsmInst::Li(low, 0x3456), AsmInst::Li(high, 0x789a)]);
        manager.set_live_after(&BTreeSet::from([1, 2]));
        code.extend(lower_unary_op(&mut manager, &mut names, IrUnaryOp::Trunc,
            &Value::Temp(1), &IrType::I8, 2));
        manager.retain_live_registers(&BTreeSet::from([1, 2]));
        let source = manager.get_register("t1".into());
        let companion = manager.get_register("source_hi".into());
        let truncated = manager.get_register("t2".into());
        code.extend(manager.take_instructions());
        let mut machine = Machine::new(64000);
        machine.run(&code);
        assert_eq!(machine.regs[source as usize], 0x3456);
        assert_eq!(machine.regs[companion as usize], 0x789a);
        assert_eq!(machine.regs[truncated as usize], 0x56);
    }
}
