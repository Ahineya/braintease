//! Main binary operation lowering logic

use rcc_frontend::ir::{Value, IrBinaryOp};
use rcc_common::TempId;
use crate::regmgmt::RegisterPressureManager;
use crate::naming::NameGenerator;
use rcc_codegen::AsmInst;
use log::{debug, trace, warn};

use crate::instr::helpers::get_value_register;
use super::{
    is_commutative, can_reuse_register, calculate_register_needs,
    generate_arithmetic_instruction, generate_comparison_instructions
};
use super::comparison::is_comparison;

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
    let result_reg = if can_reuse_register(op) && lhs_reg != rhs_reg {
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
    if let Some(inst) = generate_arithmetic_instruction(op, result_reg, lhs_reg, rhs_reg, should_swap) {
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
    
    let mut insts = vec![];
    let result_name = naming.temp_name(result_temp);
    
    // Get register for left operand
    let lhs_reg = get_value_register(mgr, naming, lhs);
    insts.extend(mgr.take_instructions());
    mgr.pin_register(lhs_reg);
    
    // Allocate result register (can often reuse lhs)
    let result_reg = mgr.get_register(result_name);
    insts.extend(mgr.take_instructions());
    
    // Handle operations with immediate values
    match op {
        // Operations with immediate instruction support
        IrBinaryOp::Add => {
            insts.push(AsmInst::AddI(result_reg, lhs_reg, rhs_const));
            trace!("  Generated ADDI instruction");
        }
        IrBinaryOp::Sub => {
            // SubI doesn't exist, use AddI with negative value
            insts.push(AsmInst::AddI(result_reg, lhs_reg, -rhs_const));
            trace!("  Generated ADDI with negated immediate for SUB");
        }
        IrBinaryOp::Mul => {
            insts.push(AsmInst::MulI(result_reg, lhs_reg, rhs_const));
            trace!("  Generated MULI instruction");
        }
        IrBinaryOp::SDiv => {
            // VM DIVI is unsigned, need to handle signs for signed division
            warn!("  SDiv with immediate not fully implemented - using unsigned division");
            warn!("  This will produce incorrect results for negative numbers!");
            insts.push(AsmInst::DivI(result_reg, lhs_reg, rhs_const));
            trace!("  Generated DIVI instruction (unsigned)");
        }
        IrBinaryOp::UDiv => {
            // VM DIVI is unsigned - direct mapping
            insts.push(AsmInst::DivI(result_reg, lhs_reg, rhs_const));
            trace!("  Generated DIVI instruction");
        }
        IrBinaryOp::SRem => {
            // VM MODI is unsigned, need to handle signs for signed remainder
            warn!("  SRem with immediate not fully implemented - using unsigned modulo");
            warn!("  This will produce incorrect results for negative numbers!");
            insts.push(AsmInst::ModI(result_reg, lhs_reg, rhs_const));
            trace!("  Generated MODI instruction (unsigned)");
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
        
        IrBinaryOp::Shl | IrBinaryOp::LShr | IrBinaryOp::AShr => {
            debug!("  No immediate form for shift, loading constant into register");
            let rhs_reg = mgr.get_register(naming.imm_value(rhs_const));
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(rhs_reg, rhs_const));
            
            match op {
                IrBinaryOp::Shl => insts.push(AsmInst::Sll(result_reg, lhs_reg, rhs_reg)),
                IrBinaryOp::LShr | IrBinaryOp::AShr => {
                    if matches!(op, IrBinaryOp::AShr) {
                        warn!("  AShr treated as logical shift right");
                    }
                    insts.push(AsmInst::Srl(result_reg, lhs_reg, rhs_reg));
                }
                _ => unreachable!(),
            }
            
            mgr.free_register(rhs_reg);
            trace!("  Generated shift with loaded immediate");
        }
        
        // Comparison operations - delegate to main binary op handler
        IrBinaryOp::Eq | IrBinaryOp::Ne | 
        IrBinaryOp::Slt | IrBinaryOp::Sle | IrBinaryOp::Sgt | IrBinaryOp::Sge |
        IrBinaryOp::Ult | IrBinaryOp::Ule | IrBinaryOp::Ugt | IrBinaryOp::Uge => {
            debug!("  Comparison with immediate - using general binary op handler");
            mgr.unpin_register(lhs_reg);
            // Free the registers we've allocated since lower_binary_op will reallocate
            mgr.free_register(lhs_reg);
            mgr.free_register(result_reg);
            
            let rhs_val = Value::Constant(rhs_const as i64);
            return lower_binary_op(mgr, naming, op, lhs, &rhs_val, result_temp);
        }
    }

    mgr.unpin_register(lhs_reg);
    
    // Free lhs register if not reused
    if result_reg != lhs_reg {
        mgr.free_register(lhs_reg);
    }
    
    debug!("lower_binary_op_immediate complete: generated {} instructions", insts.len());
    insts
}