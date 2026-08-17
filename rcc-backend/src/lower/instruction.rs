//! Instruction Lowering - Handles lowering of individual instructions
//! 
//! This module is responsible for lowering individual IR instructions to assembly,
//! delegating to specialized modules for each instruction type.

use rcc_frontend::ir::{Instruction, Value, IrType, FatPointer};
use rcc_frontend::BankTag;
use rcc_codegen::{AsmInst, Reg};
use std::collections::HashMap;
use log::{debug, warn};

use crate::naming::NameGenerator;
use crate::globals::GlobalManager;
use crate::function::{FunctionBuilder, CallArg, CallTarget};
use crate::regmgmt::{BankInfo, RegisterPressureManager};

// Import all the existing lowering functions
use crate::instr::{
    lower_load, lower_store, lower_gep,
    lower_binary_op, lower_unary_op,
    lower_inline_asm_extended,
    helpers::{resolve_global_to_fatptr, canonicalize_value}
};

/// Lower a single instruction using the existing infrastructure
pub fn lower_instruction(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    instruction: &Instruction,
    function_name: &str,
    alloca_offsets: &HashMap<rcc_common::TempId, i16>,
    global_manager: &GlobalManager,
    bank_size: u16,
    vararg_fp_offset: Option<i16>,
) -> Result<Vec<AsmInst>, String> {
    let mut insts = Vec::new();
    
    match instruction {
        Instruction::Binary { result, op, lhs, rhs, result_type } => {
            debug!("V2: Binary: t{result} = {lhs:?} {op:?} {rhs:?}");
            let use_i64 = result_type.is_i64()
                || crate::instr::i64::value_is_i64(mgr, naming, lhs)
                || crate::instr::i64::value_is_i64(mgr, naming, rhs);
            let use_i32 = result_type.is_wide()
                || crate::instr::wide::value_is_i32(mgr, naming, lhs)
                || crate::instr::wide::value_is_i32(mgr, naming, rhs);
            let binary_insts = if use_i64 {
                crate::instr::i64::lower_i64_binary(mgr, naming, *op, lhs, rhs, *result)
            } else if use_i32 {
                crate::instr::wide::lower_i32_binary(mgr, naming, *op, lhs, rhs, *result)
            } else {
                lower_binary_op(mgr, naming, *op, lhs, rhs, *result)
            };
            insts.extend(binary_insts);
        }
        
        Instruction::Unary { result, op, operand, result_type } => {
            debug!("V2: Unary: t{result} = {op:?} {operand:?}");
            let unary_insts = if result_type.is_i64()
                && matches!(op, rcc_frontend::ir::IrUnaryOp::SExt | rcc_frontend::ir::IrUnaryOp::ZExt)
            {
                crate::instr::i64::lower_i64_extend(mgr, naming, *op, operand, *result)
            } else if result_type.is_wide()
                && matches!(op, rcc_frontend::ir::IrUnaryOp::SExt | rcc_frontend::ir::IrUnaryOp::ZExt)
            {
                crate::instr::wide::lower_i32_extend(mgr, naming, *op, operand, *result)
            } else if result_type.is_wide()
                && matches!(op, rcc_frontend::ir::IrUnaryOp::Trunc)
            {
                crate::instr::i64::lower_trunc_i64_to_i32(mgr, naming, operand, *result)
            } else {
                lower_unary_op(mgr, naming, *op, operand, result_type, *result)
            };
            insts.extend(unary_insts);
        }
        
        Instruction::Load { result, ptr, result_type } => {
            debug!("V2: Load: t{result} = load {ptr:?}");
            
            insts.push(AsmInst::Comment(format!("Load instruction: t{result} = load {ptr:?}")));
            
            // Handle global variable loads specially
            let load_insts = if let Value::Global(name) = ptr {
                // Resolve global to fat pointer
                insts.push(AsmInst::Comment(format!("Canonicalizing global variable: {name}")));
                let global_ptr = resolve_global_to_fatptr(name, global_manager)?;
                lower_load(mgr, naming, &global_ptr, result_type, *result)
            } else if let Value::FatPtr(_) = ptr {
                // Canonicalize to resolve any global references
                insts.push(AsmInst::Comment(format!("Canonicalizing fat pointer: {ptr:?}")));
                let canonical_ptr = canonicalize_value(ptr, global_manager)?;
                lower_load(mgr, naming, &canonical_ptr, result_type, *result)
            } else {
                insts.push(AsmInst::Comment(format!("Canonicalizing pointer: {ptr:?}")));
                lower_load(mgr, naming, ptr, result_type, *result)
            };
            insts.extend(load_insts);
        }
        
        Instruction::Store { value, ptr, value_type } => {
            debug!("V2: Store: {value:?} -> {ptr:?}");

            // Canonicalize both value and pointer to resolve any global references
            let canon_value = canonicalize_value(value, global_manager)?;
            let canon_ptr = canonicalize_value(ptr, global_manager)?;

            // Delegate to store lowering with canonicalized operands
            let store_insts = lower_store(mgr, naming, &canon_value, &canon_ptr, value_type);
            insts.extend(store_insts);
        }
        
        Instruction::GetElementPtr { result, ptr, indices, result_type: _, source_element_type, is_struct_field } => {
            debug!("V2: GEP: t{result} = gep {source_element_type:?}, {ptr:?} + {indices:?}, is_struct_field={is_struct_field}");
            
            // CRITICAL: The is_struct_field flag tells us how to interpret the index:
            // - true: index is the pre-computed field offset in words
            // - false: index is array/pointer index that needs multiplication
            
            // Compute element size based on whether this is struct field access
            let element_size = if indices.len() == 1 {
                if *is_struct_field {
                    // For struct field access, the index IS the offset in words
                    // No multiplication needed
                    debug!("  GEP is struct field access (offset in words)");
                    1
                } else {
                    // For array indexing and pointer arithmetic,
                    // multiply index by the size of source_element_type
                    let size = source_element_type.size_in_words()
                        .and_then(|s| if s > 0 { Some(s as i16) } else { None })
                        .unwrap_or(1);
                    debug!("  GEP is array indexing/pointer arithmetic, element size = {size} words (size of {source_element_type:?})");
                    size
                }
            } else {
                // Multi-index GEP not supported
                panic!("Multi-index GEP not supported by backend");
            };
            
            // Canonicalize pointer to resolve any global references
            let canonical_ptr = canonicalize_value(ptr, global_manager)?;
            let gep_insts = lower_gep(mgr, naming, &canonical_ptr, indices, element_size, *result, bank_size);
            insts.extend(gep_insts);
        }
        
        Instruction::Alloca { result, alloc_type, .. } => {
            debug!("V2: Alloca: t{result} = alloca {alloc_type:?}");

            // Fetch the precomputed offset for this alloca
            let offset = *alloca_offsets.get(result).expect("alloca offset missing");

            // Compute pointer = FP + offset (locals live in [FP..FP+local_slots))
            let result_name = naming.temp_name(*result);
            let result_reg = mgr.get_register(result_name.clone());
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Add(result_reg, Reg::Fp, Reg::R0));
            if offset != 0 {
                insts.push(AsmInst::AddI(result_reg, result_reg, offset));
            }

            // Mark as stack pointer
            mgr.set_pointer_bank(result_name.clone(), BankInfo::Stack);
            
            // Register this alloca so it can be recomputed if needed later
            mgr.register_alloca(result_name.clone(), offset);
            
            mgr.bind_value_to_register(result_name, result_reg);

            debug!("V2: Alloca FP+{offset}");
        }
        
        Instruction::Call { result, function: func, args, result_type, stack_args_from } => {
            debug!("V2: Call: {:?} = call {}({})", result, func, args.len());
            
            // Convert IR Values to CallArgs for the calling convention
            let mut call_args = Vec::new();
            for arg in args {
                match arg {
                    Value::Temp(t) => {
                        let name = naming.temp_name(*t);
                        if let Some(words) = mgr.get_i64_words(&name) {
                            let w0 = mgr.get_register(name);
                            insts.extend(mgr.take_instructions());
                            let mut regs = [w0, Reg::R0, Reg::R0, Reg::R0];
                            for i in 0..3 {
                                regs[i + 1] = mgr.get_register(words[i].clone());
                                insts.extend(mgr.take_instructions());
                            }
                            call_args.push(CallArg::I64 { words: regs });
                        } else if let Some(hi_name) = mgr.get_i32_high(&name) {
                            let lo_reg = mgr.get_register(name);
                            insts.extend(mgr.take_instructions());
                            let hi_reg = mgr.get_register(hi_name);
                            insts.extend(mgr.take_instructions());
                            call_args.push(CallArg::FatPointer { addr: lo_reg, bank: hi_reg });
                        } else if let Some(bank_info) = mgr.get_pointer_bank(&name) {
                            let addr_reg = mgr.get_register(name.clone());
                            insts.extend(mgr.take_instructions());
                            
                            // Use the new function that preserves bank tags for arguments
                            use crate::instr::helpers::get_bank_value_for_argument;
                            let (bank_reg, check_insts) = get_bank_value_for_argument(
                                &bank_info,
                                mgr,
                                naming,
                                "call_arg"
                            );
                            insts.extend(check_insts);
                            
                            call_args.push(CallArg::FatPointer { addr: addr_reg, bank: bank_reg });
                        } else {
                            // Scalar argument
                            let reg = mgr.get_register(name);
                            insts.extend(mgr.take_instructions());
                            call_args.push(CallArg::Scalar(reg));
                        }
                    }
                    Value::Constant(c) => {
                        // Load constant into a register. Truncate to 16 bits;
                        // I32 arguments are materialized as temps in the frontend.
                        let temp_name = naming.const_value(*c);
                        let reg = mgr.get_register(temp_name);
                        insts.extend(mgr.take_instructions());
                        insts.push(AsmInst::Li(reg, *c as i16));
                        call_args.push(CallArg::Scalar(reg));
                    }
                    Value::FatPtr(fp) => {
                        // Handle fat pointer directly
                        let addr_reg = match fp.addr.as_ref() {
                            Value::Temp(t) => {
                                let name = naming.temp_name(*t);
                                mgr.get_register(name)
                            }
                            Value::Constant(c) => {
                                let temp_name = naming.const_value(*c);
                                let reg = mgr.get_register(temp_name);
                                insts.push(AsmInst::Li(reg, *c as i16));
                                reg
                            }
                            Value::Global(name) => {
                                // Resolve global and extract address
                                let global_ptr = resolve_global_to_fatptr(name, global_manager)?;
                                if let Value::FatPtr(fp) = global_ptr {
                                    if let Value::Constant(addr) = *fp.addr {
                                        let temp_name = naming.const_value(addr);
                                        let reg = mgr.get_register(temp_name);
                                        insts.push(AsmInst::Li(reg, addr as i16));
                                        reg
                                    } else {
                                        panic!("Resolved global should have constant address");
                                    }
                                } else {
                                    panic!("resolve_global_to_fatptr should return FatPtr");
                                }
                            }
                            _ => panic!("Unsupported fat pointer address type, {:?}", fp.addr),
                        };
                        insts.extend(mgr.take_instructions());
                        
                        // Use the helper function to resolve bank tag to bank info
                        // This handles all bank types including Mixed properly
                        use crate::instr::helpers::{resolve_bank_tag_to_info, get_bank_value_for_argument};
                        let bank_info = resolve_bank_tag_to_info(&fp.bank, fp, mgr, naming);
                        
                        // Use the new function that preserves bank tags for arguments
                        let (bank_reg, check_insts) = get_bank_value_for_argument(
                            &bank_info,
                            mgr,
                            naming,
                            "call_arg_fp"
                        );
                        insts.extend(check_insts);
                        
                        call_args.push(CallArg::FatPointer { addr: addr_reg, bank: bank_reg });
                    }
                    _ => {
                        warn!("V2: Unsupported argument type for call: {arg:?}");
                    }
                }
            }
            
            // Extract function name for label-based calls
            let func_name = match func {
                Value::Function(name) | Value::Global(name) => name.clone(),
                _ => return Err(format!("V2: Invalid function value for call: {func:?}")),
            };
            
            // Prepare the result name if there's a return value
            let result_name = result.map(|id| naming.temp_name(id));
            
            // Use FunctionBuilder's standalone call method
            // This handles everything: argument setup, call, return value binding, and stack cleanup
            let call_insts = FunctionBuilder::make_standalone_call(
                mgr,
                naming,
                CallTarget::Label(func_name.clone()),
                call_args,
                result_type.is_pointer(),
                result_type.is_wide(),
                result_type.is_i64(),
                result_name,
                *stack_args_from,
            );
            
            insts.extend(call_insts);
            
            // Return value binding is now handled inside make_complete_call_by_label!
            
            debug!("V2: Call lowered with {} arguments", args.len());
        }
        
        Instruction::Return(_) => {
            // Return instructions are handled specially by the FunctionBuilder
            // in lower_function_v2. If we get here, it's a compiler bug.
            panic!("V2: COMPILER BUG: Return instruction reached lower_instruction. All Returns should be handled by FunctionBuilder!");
        }
        
        Instruction::Branch(label) => {
            debug!("V2: Branch to label {label}");
            // Get the proper label name with function context
            let label_name = naming.block_label(function_name, *label);
            // Create a simple unconditional branch
            insts.push(AsmInst::Beq(Reg::R0, Reg::R0, label_name.clone()));
            insts.push(AsmInst::Comment(format!("Unconditional branch to {label_name}")));
        }
        
        Instruction::BranchCond { condition, true_label, false_label } => {
            debug!("V2: Conditional branch: {condition} ? {true_label} : {false_label}");
            // Get the proper label names with function context
            let true_label_name = naming.block_label(function_name, *true_label);
            let false_label_name = naming.block_label(function_name, *false_label);
            
            // Get register for condition value (I32: OR both words)
            let (cond_reg, cond_insts) = crate::instr::wide::i32_to_cond_reg(mgr, naming, condition);
            insts.extend(cond_insts);
            
            // Branch if condition is zero (false) to false_label
            insts.push(AsmInst::Beq(cond_reg, Reg::R0, false_label_name.clone()));
            insts.push(AsmInst::Comment(format!("Branch to {false_label_name} if condition is false")));
            
            // If condition was non-zero (true), branch to true_label
            insts.push(AsmInst::Beq(Reg::R0, Reg::R0, true_label_name.clone()));
            insts.push(AsmInst::Comment(format!("Unconditional branch to {true_label_name} (condition was true)")));
            
            // Free the condition register
            mgr.free_register(cond_reg);
        }
        
        Instruction::Phi { result, incoming, .. } => {
            debug!("V2: Phi: t{} with {} incoming", result, incoming.len());
            // Phi nodes require SSA deconstruction - simplified for now
            warn!("V2: Phi nodes not fully implemented - using first value");
            if let Some((value, _label)) = incoming.first() {
                match value {
                    Value::Temp(t) => {
                        let src_name = naming.temp_name(*t);
                        let dst_name = naming.temp_name(*result);
                        let src_reg = mgr.get_register(src_name);
                        let dst_reg = mgr.get_register(dst_name);
                        insts.extend(mgr.take_instructions());
                        insts.push(AsmInst::Move(dst_reg, src_reg));
                        mgr.free_register(src_reg);
                    }
                    Value::Constant(c) => {
                        let dst_name = naming.temp_name(*result);
                        let dst_reg = mgr.get_register(dst_name);
                        insts.extend(mgr.take_instructions());
                        insts.push(AsmInst::Li(dst_reg, *c as i16));
                    }
                    _ => {}
                }
            }
        }
        
        Instruction::Select { result, condition, true_value, false_value, .. } => {
            debug!("V2: Select: t{result} = {condition} ? {true_value} : {false_value}");
            
            // Get condition value
            let cond_reg = match condition {
                Value::Temp(t) => {
                    let name = naming.temp_name(*t);
                    mgr.get_register(name)
                }
                Value::Constant(c) => {
                    let temp_name = naming.const_value(*c);
                    let reg = mgr.get_register(temp_name);
                    insts.extend(mgr.take_instructions());
                    insts.push(AsmInst::Li(reg, *c as i16));
                    reg
                }
                _ => return Err(format!("V2: Unsupported condition type for select: {condition:?}")),
            };
            insts.extend(mgr.take_instructions());
            
            // Generate labels using naming module
            let true_label = naming.select_true_label(*result);
            let end_label = naming.select_end_label(*result);
            
            // Branch if condition is non-zero
            insts.push(AsmInst::Bne(cond_reg, Reg::R0, true_label.clone()));
            
            // False case
            let result_name = naming.temp_name(*result);
            let result_reg = mgr.get_register(result_name);
            insts.extend(mgr.take_instructions());
            
            match false_value {
                Value::Temp(t) => {
                    let src_name = naming.temp_name(*t);
                    let src_reg = mgr.get_register(src_name);
                    insts.extend(mgr.take_instructions());
                    insts.push(AsmInst::Move(result_reg, src_reg));
                    mgr.free_register(src_reg);
                }
                Value::Constant(c) => {
                    insts.push(AsmInst::Li(result_reg, *c as i16));
                }
                _ => {}
            }
            
            // Jump to end
            insts.push(AsmInst::Beq(Reg::R0, Reg::R0, end_label.clone()));
            
            // True case
            insts.push(AsmInst::Label(true_label));
            match true_value {
                Value::Temp(t) => {
                    let src_name = naming.temp_name(*t);
                    let src_reg = mgr.get_register(src_name);
                    insts.extend(mgr.take_instructions());
                    insts.push(AsmInst::Move(result_reg, src_reg));
                    mgr.free_register(src_reg);
                }
                Value::Constant(c) => {
                    insts.push(AsmInst::Li(result_reg, *c as i16));
                }
                _ => {}
            }
            
            // End label
            insts.push(AsmInst::Label(end_label));
            
            mgr.free_register(cond_reg);
        }
        
        Instruction::Cast { result, value, target_type } => {
            debug!("V2: Cast: t{result} = cast {value:?} to {target_type:?}");
            if target_type.is_i64() {
                insts.extend(crate::instr::i64::lower_i64_extend(
                    mgr, naming, rcc_frontend::ir::IrUnaryOp::SExt, value, *result,
                ));
            } else if target_type.is_wide() {
                insts.extend(crate::instr::wide::lower_i32_extend(
                    mgr, naming, rcc_frontend::ir::IrUnaryOp::SExt, value, *result,
                ));
            } else if let Value::Temp(t) = value {
                let src_name = naming.temp_name(*t);
                let dst_name = naming.temp_name(*result);
                let src_reg = mgr.get_register(src_name);
                let dst_reg = mgr.get_register(dst_name);
                insts.extend(mgr.take_instructions());
                insts.push(AsmInst::Move(dst_reg, src_reg));
                mgr.free_register(src_reg);
            } else if let Value::Constant(c) = value {
                let dst_name = naming.temp_name(*result);
                let dst_reg = mgr.get_register(dst_name);
                insts.extend(mgr.take_instructions());
                insts.push(AsmInst::Li(dst_reg, *c as i16));
            }
        }
        
        Instruction::Intrinsic { result, intrinsic, args, result_type } => {
            match intrinsic.as_str() {
                "va_start" => {
                    let offset = vararg_fp_offset.ok_or_else(|| {
                        "va_start used in a non-variadic function".to_string()
                    })?;
                    if args.is_empty() {
                        return Err("va_start requires the address of the va_list".to_string());
                    }
                    let ap_ptr = canonicalize_value(&args[0], global_manager)?;
                    insts.extend(lower_va_start(mgr, naming, &ap_ptr, offset));
                }
                "va_arg" => {
                    if args.is_empty() {
                        return Err("va_arg requires the address of the va_list".to_string());
                    }
                    let ap_ptr = canonicalize_value(&args[0], global_manager)?;
                    let dest = result.ok_or_else(|| "va_arg must produce a result".to_string())?;
                    insts.extend(lower_va_arg(mgr, naming, &ap_ptr, result_type, dest));
                }
                other => {
                    return Err(format!("Unknown intrinsic '{other}'"));
                }
            }
        }
        
        Instruction::DebugLoc { .. } => {
            // Debug info - no code generated
        }

        Instruction::InlineAsm { assembly, outputs, inputs, clobbers } => {
            debug!("V2: InlineAsm with {} outputs, {} inputs, {} clobbers", 
                   outputs.len(), inputs.len(), clobbers.len());
            
            // Always use the extended handler - it handles both cases
            match lower_inline_asm_extended(mgr, naming, assembly, outputs, inputs, clobbers) {
                Ok(asm_insts) => insts.extend(asm_insts),
                Err(e) => {
                    // This is a compilation error - we cannot continue
                    return Err(e.to_string());
                }
            }
        }
        
        Instruction::Comment(text) => {
            insts.push(AsmInst::Comment(text.clone()));
        }
    }
    
    Ok(insts)
}

fn lower_va_start(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    ap_ptr: &Value,
    offset: i16,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    insts.push(AsmInst::Comment(format!("va_start: first extra at FP{offset}")));

    let fake: rcc_common::TempId = 0x100000 + naming.next_operation_id();
    let fake_name = naming.temp_name(fake);
    let addr_reg = mgr.get_register(fake_name.clone());
    insts.extend(mgr.take_instructions());
    insts.push(AsmInst::AddI(addr_reg, Reg::Fp, offset));
    mgr.bind_value_to_register(fake_name.clone(), addr_reg);
    mgr.set_pointer_bank(fake_name, BankInfo::Stack);

    let value = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Temp(fake)),
        bank: BankTag::Stack,
    });
    insts.extend(lower_store(mgr, naming, &value, ap_ptr, &IrType::FatPtr(Box::new(IrType::I16))));
    insts
}

fn lower_va_arg(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    ap_ptr: &Value,
    result_type: &IrType,
    dest: rcc_common::TempId,
) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    insts.push(AsmInst::Comment(format!("va_arg {result_type}")));

    // Load current va_list (fat pointer to the next extra argument).
    let ap_temp: rcc_common::TempId = 0x200000 + naming.next_operation_id();
    insts.extend(lower_load(
        mgr,
        naming,
        ap_ptr,
        &IrType::FatPtr(Box::new(IrType::I16)),
        ap_temp,
    ));
    let ap_val = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Temp(ap_temp)),
        bank: BankTag::Mixed,
    });

    let words = result_type.size_in_words().unwrap_or(1) as i16;

    if result_type.is_i64() {
        let ap_name = naming.temp_name(ap_temp);
        let ap_reg = mgr.get_register(ap_name);
        insts.extend(mgr.take_instructions());
        let dest_name = naming.temp_name(dest);
        mgr.pin_register(ap_reg);
        let dest_reg = mgr.get_register(dest_name.clone());
        insts.extend(mgr.take_instructions());
        insts.push(AsmInst::Load(dest_reg, Reg::Sb, ap_reg));
        let mut extra = [String::new(), String::new(), String::new()];
        for i in 0..3 {
            extra[i] = naming.i64_word_name(&dest_name, (i + 1) as u8);
            let r = mgr.get_register(extra[i].clone());
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::AddI(Reg::Sc, ap_reg, -1 - i as i16));
            insts.push(AsmInst::Load(r, Reg::Sb, Reg::Sc));
            mgr.bind_value_to_register(extra[i].clone(), r);
        }
        mgr.set_i64_words(dest_name.clone(), extra);
        mgr.bind_value_to_register(dest_name, dest_reg);
        mgr.unpin_register(ap_reg);
    } else if result_type.is_pointer() || result_type.is_wide() {
        // Stack extras: low/addr word at ap, high/bank word at ap-1 (see load_param).
        let ap_name = naming.temp_name(ap_temp);
        let ap_reg = mgr.get_register(ap_name);
        insts.extend(mgr.take_instructions());

        let dest_name = naming.temp_name(dest);
        mgr.pin_register(ap_reg);
        let dest_reg = mgr.get_register(dest_name.clone());
        insts.extend(mgr.take_instructions());
        insts.push(AsmInst::Load(dest_reg, Reg::Sb, ap_reg));

        insts.push(AsmInst::AddI(Reg::Sc, ap_reg, -1));
        if result_type.is_wide() {
            let hi_name = naming.i32_high_name(&dest_name);
            let hi_reg = mgr.get_register(hi_name.clone());
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Load(hi_reg, Reg::Sb, Reg::Sc));
            mgr.bind_value_to_register(hi_name.clone(), hi_reg);
            mgr.set_i32_high(dest_name.clone(), hi_name);
        } else {
            let bank_name = naming.load_bank_value(dest);
            let bank_reg = mgr.get_register(bank_name.clone());
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Load(bank_reg, Reg::Sb, Reg::Sc));
            mgr.bind_value_to_register(bank_name.clone(), bank_reg);
            mgr.set_pointer_bank(dest_name, BankInfo::Dynamic(bank_name));
        }
        mgr.bind_value_to_register(naming.temp_name(dest), dest_reg);
        mgr.unpin_register(ap_reg);
    } else {
        insts.extend(lower_load(mgr, naming, &ap_val, result_type, dest));
    }

    // Walk downward: first extra is closest to FP (highest address among extras).
    let ap_name = naming.temp_name(ap_temp);
    let ap_reg = mgr.get_register(ap_name.clone());
    insts.extend(mgr.take_instructions());
    insts.push(AsmInst::AddI(ap_reg, ap_reg, -words));
    mgr.bind_value_to_register(ap_name, ap_reg);

    let updated = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Temp(ap_temp)),
        bank: BankTag::Mixed,
    });
    insts.extend(lower_store(mgr, naming, &updated, ap_ptr, &IrType::FatPtr(Box::new(IrType::I16))));
    insts
}