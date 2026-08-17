//! Unary operation lowering for V2 backend
//! 
//! Implements all unary operations including negation, bitwise NOT,
//! and type conversions (zero/sign extend, truncate, pointer casts).

use rcc_frontend::ir::{Value, IrUnaryOp, IrType};
use rcc_common::TempId;
use crate::regmgmt::RegisterPressureManager;
use crate::naming::NameGenerator;
use crate::instr::helpers::get_value_register;
use rcc_codegen::{AsmInst, Reg};
use log::{debug, trace, warn};

/// Lower a unary operation to assembly instructions
/// 
/// # Parameters
/// - `mgr`: Register pressure manager for allocation and spilling
/// - `naming`: Name generator for unique temporary names
/// - `op`: The unary operation to perform
/// - `operand`: The operand value
/// - `result_type`: Type of the result
/// - `result_temp`: Temp ID for the result
/// 
/// # Returns
/// Vector of assembly instructions for the unary operation
pub fn lower_unary_op(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    op: IrUnaryOp,
    operand: &Value,
    result_type: &IrType,
    result_temp: TempId,
) -> Vec<AsmInst> {
    debug!("lower_unary_op: op={op:?}, operand={operand:?}, result_type={result_type:?}, result=t{result_temp}");
    trace!("  Current register state: spill_count={}", mgr.get_spill_count());
    
    let mut insts = vec![];
    let result_name = naming.temp_name(result_temp);
    
    // Get register for operand
    let operand_reg = get_value_register(mgr, naming, operand);
    insts.extend(mgr.take_instructions());
    trace!("  Operand in {operand_reg:?}");
    
    // Allocate result register (can often reuse operand register for some operations)
    let result_reg = if can_reuse_register(op) {
        trace!("  Reusing operand register {operand_reg:?} for result");
        operand_reg
    } else {
        let reg = mgr.get_register(result_name.clone());
        insts.extend(mgr.take_instructions());
        trace!("  Allocated new register {reg:?} for result");
        reg
    };
    
    // Generate the appropriate instruction(s)
    match op {
        IrUnaryOp::Not => {
            // Bitwise NOT: result = ~operand
            // In RISC architectures, NOT is often XOR with -1 (all ones)
            let all_ones_reg = mgr.get_register(naming.all_ones());
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(all_ones_reg, -1)); // Load -1 (0xFFFF)
            insts.push(AsmInst::Xor(result_reg, operand_reg, all_ones_reg));
            mgr.free_register(all_ones_reg);
            trace!("  Generated NOT using XOR with -1");
        }
        
        IrUnaryOp::Neg => {
            // Arithmetic negation: result = -operand
            // Implement as: result = 0 - operand
            let zero_reg = mgr.get_register(naming.zero_temp());
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(zero_reg, 0));
            insts.push(AsmInst::Sub(result_reg, zero_reg, operand_reg));
            mgr.free_register(zero_reg);
            trace!("  Generated NEG using 0 - operand");
        }
        
        IrUnaryOp::ZExt => {
            // Zero extend - for 16-bit architecture, this is often a no-op
            // since we're already working with 16-bit values
            // Just move the value if needed
            if result_reg != operand_reg {
                insts.push(AsmInst::Add(result_reg, operand_reg, Reg::R0)); // MOV using ADD with 0
                trace!("  Generated ZEXT (move to result register)");
            } else {
                trace!("  ZEXT is no-op (same register)");
            }
        }
        
        IrUnaryOp::SExt => {
            // Sign extend - need to check the sign bit and extend appropriately
            // For now, treat as move since we're on 16-bit architecture
            // TODO: Implement proper sign extension if needed for smaller types
            warn!("  SExt not fully implemented - treating as move");
            if result_reg != operand_reg {
                insts.push(AsmInst::Add(result_reg, operand_reg, Reg::R0)); // MOV
                trace!("  Generated SEXT (move to result register)");
            }
        }
        
        IrUnaryOp::Trunc => {
            // Truncate - mask off higher bits if needed
            // For I32 → I16, the low word is already in operand_reg
            match result_type {
                IrType::I8 => {
                    // Truncate to 8 bits
                    let mask_reg = mgr.get_register(naming.mask_i8());
                    insts.extend(mgr.take_instructions());
                    insts.push(AsmInst::Li(mask_reg, 0xFF)); // 8-bit mask
                    insts.push(AsmInst::And(result_reg, operand_reg, mask_reg));
                    mgr.free_register(mask_reg);
                    trace!("  Generated TRUNC to i8 using AND 0xFF");
                }
                IrType::I1 => {
                    // Truncate to 1 bit (boolean)
                    let mask_reg = mgr.get_register(naming.mask_i1());
                    insts.extend(mgr.take_instructions());
                    insts.push(AsmInst::Li(mask_reg, 1)); // 1-bit mask
                    insts.push(AsmInst::And(result_reg, operand_reg, mask_reg));
                    mgr.free_register(mask_reg);
                    trace!("  Generated TRUNC to i1 using AND 0x1");
                }
                _ => {
                    // For I32→I16, I16→I16, just keep the low word
                    if result_reg != operand_reg {
                        insts.push(AsmInst::Add(result_reg, operand_reg, Reg::R0)); // MOV
                    }
                    trace!("  TRUNC is move/no-op for target type {result_type:?}");
                }
            }
        }
        
        IrUnaryOp::PtrToInt => {
            // Pointer to integer cast - just treat the pointer address as an integer
            // Fat pointers: only use the address part, ignore the bank
            if result_reg != operand_reg {
                insts.push(AsmInst::Add(result_reg, operand_reg, Reg::R0)); // MOV
            }
            trace!("  PtrToInt: treating pointer address as integer");
        }
        
        IrUnaryOp::IntToPtr => {
            // Integer to pointer cast - treat integer as pointer address
            // For fat pointers, we'd need to set a default bank (Gp register)
            // But since this is just the address component, we handle it as a move
            if result_reg != operand_reg {
                insts.push(AsmInst::Add(result_reg, operand_reg, Reg::R0)); // MOV
            }
            trace!("  IntToPtr: treating integer as pointer address");
            // Note: The bank component would be handled separately by the caller
        }
    }

    // Trunc/ZExt/SExt may reuse the operand register. The result temp is a
    // different name; without this bind, later uses of the result allocate a
    // fresh uninitialized register (seen when truncating long constants to int).
    mgr.bind_value_to_register(result_name, result_reg);

    // Free operand register if not reused
    if result_reg != operand_reg {
        mgr.free_register(operand_reg);
        trace!("  Freed operand register {operand_reg:?}");
    }
    
    debug!("lower_unary_op complete: generated {} instructions", insts.len());
    trace!("  Final register state: spill_count={}", mgr.get_spill_count());
    
    insts
}

/// Check if we can reuse the operand's register for the result
fn can_reuse_register(op: IrUnaryOp) -> bool {
    // Most unary operations can reuse the operand register
    // since they only have one input
    match op {
        IrUnaryOp::Not | IrUnaryOp::Neg => false, // Need both registers during operation
        _ => true, // Conversions can often reuse the register
    }
}