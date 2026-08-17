//! IR Function Lowering - Converts IR functions to assembly
//! 
//! This is the main entry point for lowering complete IR functions to assembly.
//! It uses FunctionBuilder to handle all the complexity of function generation.
//! 
//! DO NOT CONFUSE WITH /v2/function/internal.rs which is an internal implementation detail.

use rcc_frontend::ir::{Function, Instruction, Value, IrType};
use rcc_frontend::BankTag;
use rcc_codegen::{AsmInst, Reg};
use std::collections::HashMap;
use log::{debug, info, trace};
use crate::function::FunctionBuilder;
use crate::globals::GlobalManager;
use crate::instr::helpers::get_bank_register_with_runtime_check_safe;
use crate::naming::NameGenerator;
use crate::regmgmt::RegisterPressureManager;
use super::instruction::lower_instruction;

/// Compute stack offsets (relative to FP) for each alloca result
pub fn compute_alloca_offsets(function: &Function) -> HashMap<rcc_common::TempId, i16> {
    let mut offsets = HashMap::new();
    let mut cur: i16 = 0;
    for block in &function.blocks {
        for inst in &block.instructions {
            if let Instruction::Alloca { result, alloc_type, count, .. } = inst {
                let type_size = alloc_type.size_in_words().unwrap_or(1) as i16;
                let total_size = if let Some(Value::Constant(n)) = count {
                    type_size * (*n as i16)
                } else {
                    type_size
                };
                // Assign the lowest available offset for this alloca
                offsets.insert(*result, cur);
                cur += total_size;
            }
        }
    }
    offsets
}

/// Calculate the number of local slots needed for a function
pub fn calculate_local_slots(function: &Function) -> i16 {
    // Count allocas and other locals
    let mut slots = 0i16;
    
    for block in &function.blocks {
        for instruction in &block.instructions {
            if let Instruction::Alloca { alloc_type, count, .. } = instruction {
                let type_size = alloc_type.size_in_words().unwrap_or(1) as i16;
                let total_size = if let Some(Value::Constant(n)) = count {
                    type_size * (*n as i16)
                } else {
                    type_size
                };
                slots += total_size;
            }
        }
    }
    
    // Add some buffer for temporaries
    slots + 8
}

/// Setup function builder with proper parameter types
fn setup_function_builder(function: &Function) -> FunctionBuilder {
    debug!("Setting up function builder for '{}'" , function.name);
    let param_types: Vec<(rcc_common::TempId, IrType)> = function.parameters
        .iter()
        .map(|(id, ty)| (*id, ty.clone()))
        .collect();
    
    FunctionBuilder::with_params(param_types)
}

/// Bind function parameters to registers or stack locations
fn bind_function_parameters(
    function: &Function,
    builder: &mut FunctionBuilder,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
) {
    debug!("Binding {} function parameters", function.parameters.len());
    for (idx, (param_id, _ty)) in function.parameters.iter().enumerate() {
        // Use the builder to load and bind the parameter properly
        let (_addr_reg, _bank_reg) = builder.load_parameter_with_binding(idx, *param_id, mgr, naming);
        trace!("  Bound parameter {idx} (t{param_id})");
    }
}

/// Generate labels for all basic blocks in the function
fn generate_block_labels(
    function: &Function,
    naming: &mut NameGenerator,
) -> HashMap<u32, String> {
    debug!("Generating labels for {} blocks", function.blocks.len());
    let mut block_labels = HashMap::new();
    for block in function.blocks.iter() {
        let label = naming.block_label(&function.name, block.id);
        trace!("  Block {} -> {}", block.id, label);
        block_labels.insert(block.id, label);
    }
    block_labels
}

/// Handle a return instruction, preparing return values and jumping to epilogue
fn handle_return_instruction(
    value: &Option<Value>,
    function: &Function,
    builder: &mut FunctionBuilder,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    epilogue_label: &str,
) -> Option<(Reg, Option<Reg>)> {
    debug!("Handling return instruction");
    
    // Prepare return value if any
    let return_regs = if let Some(val) = value {
        match val {
            Value::Temp(t) => {
                let temp_name = naming.temp_name(*t);
                
                // Check if returning a pointer (would need bank too)
                if function.return_type.is_pointer() {
                    // Get the bank register for this pointer first, before moving temp_name
                    let bank_info = mgr.get_pointer_bank(&temp_name)
                        .unwrap_or_else(|| {
                            // This is a compiler bug - all pointers must have bank info
                            panic!("V2: COMPILER BUG: No bank info for pointer return value t{t}. All pointers must have tracked bank information!");
                        });
                    let (bank_reg, check_insts) = get_bank_register_with_runtime_check_safe(&bank_info, mgr, naming, "return_pointer");
                    builder.add_instructions(check_insts);
                    
                    let temp_reg = mgr.get_register(temp_name);
                    builder.add_instructions(mgr.take_instructions());
                    
                    // Move to return registers if needed
                    if temp_reg != Reg::Rv0 {
                        builder.add_instruction(AsmInst::Move(Reg::Rv0, temp_reg));
                    }
                    if bank_reg != Reg::Rv1 {
                        builder.add_instruction(AsmInst::Move(Reg::Rv1, bank_reg));
                    }
                    trace!("  Returning fat pointer in Rv0/Rv1");
                    Some((Reg::Rv0, Some(Reg::Rv1)))
                } else if function.return_type.is_i64() {
                    let temp_reg = mgr.get_register(temp_name.clone());
                    builder.add_instructions(mgr.take_instructions());
                    if temp_reg != Reg::Rv0 {
                        builder.add_instruction(AsmInst::Move(Reg::Rv0, temp_reg));
                    }
                    if let Some(words) = mgr.get_i64_words(&temp_name) {
                        let dests = [Reg::Rv1, Reg::X0, Reg::X1];
                        for i in 0..3 {
                            let r = mgr.get_register(words[i].clone());
                            builder.add_instructions(mgr.take_instructions());
                            if r != dests[i] {
                                builder.add_instruction(AsmInst::Move(dests[i], r));
                            }
                        }
                    } else {
                        builder.add_instruction(AsmInst::Li(Reg::Rv1, 0));
                        builder.add_instruction(AsmInst::Li(Reg::X0, 0));
                        builder.add_instruction(AsmInst::Li(Reg::X1, 0));
                    }
                    trace!("  Returning I64 in Rv0/Rv1/X0/X1");
                    Some((Reg::Rv0, Some(Reg::Rv1)))
                } else if function.return_type.is_wide() {
                    let temp_reg = mgr.get_register(temp_name.clone());
                    builder.add_instructions(mgr.take_instructions());
                    if temp_reg != Reg::Rv0 {
                        builder.add_instruction(AsmInst::Move(Reg::Rv0, temp_reg));
                    }
                    if let Some(hi_name) = mgr.get_i32_high(&temp_name) {
                        let hi_reg = mgr.get_register(hi_name);
                        builder.add_instructions(mgr.take_instructions());
                        if hi_reg != Reg::Rv1 {
                            builder.add_instruction(AsmInst::Move(Reg::Rv1, hi_reg));
                        }
                    } else {
                        builder.add_instruction(AsmInst::Li(Reg::Sc, 15));
                        builder.add_instruction(AsmInst::Srl(Reg::Rv1, Reg::Rv0, Reg::Sc));
                        builder.add_instruction(AsmInst::Sub(Reg::Rv1, Reg::R0, Reg::Rv1));
                    }
                    trace!("  Returning I32 in Rv0/Rv1");
                    Some((Reg::Rv0, Some(Reg::Rv1)))
                } else {
                    let temp_reg = mgr.get_register(temp_name);
                    builder.add_instructions(mgr.take_instructions());
                    
                    // Move to return register if needed
                    if temp_reg != Reg::Rv0 {
                        builder.add_instruction(AsmInst::Move(Reg::Rv0, temp_reg));
                    }
                    trace!("  Returning scalar in Rv0");
                    Some((Reg::Rv0, None))
                }
            }
            Value::Constant(c) => {
                if function.return_type.is_i64() {
                    let parts = crate::instr::i64::split_const64(*c);
                    builder.add_instruction(AsmInst::Li(Reg::Rv0, parts[0]));
                    builder.add_instruction(AsmInst::Li(Reg::Rv1, parts[1]));
                    builder.add_instruction(AsmInst::Li(Reg::X0, parts[2]));
                    builder.add_instruction(AsmInst::Li(Reg::X1, parts[3]));
                    trace!("  Returning I64 constant {c} in Rv0/Rv1/X0/X1");
                    Some((Reg::Rv0, Some(Reg::Rv1)))
                } else if function.return_type.is_wide() {
                    let (lo, hi) = crate::instr::wide::split_const(*c);
                    builder.add_instruction(AsmInst::Li(Reg::Rv0, lo));
                    builder.add_instruction(AsmInst::Li(Reg::Rv1, hi));
                    trace!("  Returning I32 constant {c} in Rv0/Rv1");
                    Some((Reg::Rv0, Some(Reg::Rv1)))
                } else {
                    // Load constant directly into return register
                    builder.add_instruction(AsmInst::Li(Reg::Rv0, *c as i16));
                    trace!("  Returning constant {c} in Rv0");
                    Some((Reg::Rv0, None))
                }
            }
            Value::FatPtr(fp) => {
                // Handle fat pointer return
                // Get the address part
                let addr_reg = match fp.addr.as_ref() {
                    Value::Temp(t) => {
                        let temp_name = naming.temp_name(*t);
                        mgr.get_register(temp_name)
                    }
                    Value::Constant(c) => {
                        builder.add_instruction(AsmInst::Li(Reg::Rv0, *c as i16));
                        Reg::Rv0
                    }
                    _ => panic!("Unexpected value type in FatPtr address: {:?}", fp.addr),
                };
                
                // Get the bank part
                let bank_reg = match fp.bank {
                    BankTag::Global => {
                        builder.add_instruction(AsmInst::Move(Reg::Rv1, Reg::Gp));
                        Reg::Rv1
                    }
                    BankTag::Stack => {
                        builder.add_instruction(AsmInst::Move(Reg::Rv1, Reg::Sb));
                        Reg::Rv1
                    }
                    BankTag::Mixed => {
                        // For Mixed, the bank should be tracked in the register manager
                        if let Value::Temp(t) = fp.addr.as_ref() {
                            let temp_name = naming.temp_name(*t);
                            let bank_info = mgr.get_pointer_bank(&temp_name)
                                .unwrap_or_else(|| {
                                    panic!("No bank info for Mixed pointer t{t}");
                                });
                            let (bank_reg, check_insts) = get_bank_register_with_runtime_check_safe(&bank_info, mgr, naming, "return_mixed_pointer");
                            builder.add_instructions(check_insts);
                            bank_reg
                        } else {
                            panic!("Mixed bank requires temp value");
                        }
                    }
                    BankTag::Null => {
                        // NULL pointer - return 0 for bank
                        let bank_name = naming.temp_with_context("return", "null_bank");
                        let bank_reg = mgr.get_register(bank_name);
                        builder.add_instructions(mgr.take_instructions());
                        builder.add_instruction(AsmInst::Li(bank_reg, 0));
                        bank_reg
                    }
                    BankTag::Heap(bank) => {
                        // Heap with specific bank number
                        let bank_name = naming.temp_with_context("return", "heap_bank");
                        let bank_reg = mgr.get_register(bank_name);
                        builder.add_instructions(mgr.take_instructions());
                        builder.add_instruction(AsmInst::Li(bank_reg, bank as i16));
                        bank_reg
                    }
                    _ => panic!("Unsupported bank tag for return: {:?}", fp.bank),
                };
                
                builder.add_instructions(mgr.take_instructions());
                
                // Move to return registers if needed
                if addr_reg != Reg::Rv0 {
                    builder.add_instruction(AsmInst::Move(Reg::Rv0, addr_reg));
                }
                if bank_reg != Reg::Rv1 {
                    builder.add_instruction(AsmInst::Move(Reg::Rv1, bank_reg));
                }
                
                trace!("  Returning fat pointer in Rv0/Rv1");
                Some((Reg::Rv0, Some(Reg::Rv1)))
            }
            _ => {
                trace!("  Void return");
                None
            }
        }
    } else {
        trace!("  Void return");
        None
    };
    
    // Jump to common epilogue
    builder.add_instruction(AsmInst::Comment("Jump to epilogue".to_string()));
    builder.add_instruction(AsmInst::Beq(Reg::R0, Reg::R0, epilogue_label.to_string()));
    
    return_regs
}

/// Lower a single basic block's instructions
fn lower_basic_block(
    block: &rcc_frontend::ir::BasicBlock,
    block_idx: usize,
    function: &Function,
    builder: &mut FunctionBuilder,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    block_labels: &HashMap<u32, String>,
    alloca_offsets: &HashMap<rcc_common::TempId, i16>,
    global_manager: &GlobalManager,
    epilogue_label: &str,
    bank_size: u16,
    vararg_fp_offset: Option<i16>,
) -> Result<Vec<(Option<(Reg, Option<Reg>)>, usize)>, String> {
    debug!("Lowering block {} (index {}) [loop_condition={}]", block.id, block_idx, block.is_loop_condition);
    
    // For loop condition blocks, we need special handling to preserve bank bindings
    // The issue is that short-circuit evaluation in compound conditions creates
    // complex control flow that causes register spilling. When we reload these
    // spilled values, we lose the bank binding information.
    if block.is_loop_condition {
        debug!("  This is a loop condition block - preserving bank bindings");
        // Don't invalidate bank bindings for loop condition blocks
        // This ensures that GEP-computed bank values are preserved across loop iterations
        mgr.invalidate_alloca_bindings();
    } else {
        // Invalidate alloca-register bindings at block boundaries
        // This ensures allocas are always recomputed fresh in loop headers,
        // preventing issues where a register containing an alloca address
        // gets overwritten in the loop body and isn't restored.
        mgr.invalidate_alloca_bindings();
        
        // DO NOT invalidate GEP bank bindings at block boundaries.
        // The bank of a pointer is a property of the pointer value, not the register,
        // and doesn't change during execution. Invalidating causes issues in loops
        // where GEP operations regenerate bank checking code unnecessarily.
        // Previous code: mgr.invalidate_gep_bank_bindings();
        // This was causing string literal dereferencing to fail in loops.
    }
    
    // Take any instructions generated by the invalidation
    let invalidation_insts = mgr.take_instructions();
    builder.add_instructions(invalidation_insts);
    
    // Add label for the block (except for entry block which is implicit)
    if block.id != 0 {
        let label_name = block_labels.get(&block.id).unwrap().clone();
        builder.add_instruction(AsmInst::Label(label_name));
    }
    
    let mut return_values = Vec::new();
    
    // Lower each instruction in the block
    for instruction in &block.instructions {
        trace!("  Lowering instruction: {instruction:?}");
        
        match instruction {
            Instruction::Return(value) => {
                let return_regs = handle_return_instruction(
                    value,
                    function,
                    builder,
                    mgr,
                    naming,
                    epilogue_label,
                );
                return_values.push((return_regs, block_idx));
            }
            
            _ => {
                // Use the existing lower_instruction for other instructions
                let insts = lower_instruction(mgr, naming, instruction, &function.name, alloca_offsets, global_manager, bank_size, vararg_fp_offset)?;
                builder.add_instructions(insts);
            }
        }
    }
    
    Ok(return_values)
}

/// Generate the function epilogue based on return values
fn generate_epilogue(
    builder: &mut FunctionBuilder,
    has_any_return: bool,
    return_values: &[(Option<(Reg, Option<Reg>)>, usize)],
    epilogue_label: String,
) {
    debug!("Generating epilogue (has_return: {has_any_return})");
    
    if has_any_return {
        builder.add_instruction(AsmInst::Label(epilogue_label));
        // Use the first return value format (they should all be consistent)
        let return_regs = if !return_values.is_empty() {
            return_values[0].0
        } else {
            None
        };
        builder.end_function(return_regs);
    } else {
        // No explicit returns, add default return at the end
        builder.end_function(None);
    }
}

/// Lower a complete function using the V2 backend
pub fn lower_function_v2(
    function: &Function, 
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    global_manager: &GlobalManager,
    bank_size: u16
) -> Result<Vec<AsmInst>, String> {
    info!("V2: Lowering function '{}' with {} blocks", function.name, function.blocks.len());
    
    let mut instructions = Vec::new();
    
    // Add function label first
    instructions.push(AsmInst::Label(function.name.clone()));
    
    // Setup function builder
    let mut builder = setup_function_builder(function);
    
    // Get local slots from the manager (already calculated in module.rs)
    let local_slots = mgr.local_count();
    let alloca_offsets = compute_alloca_offsets(function);
    
    // Begin the function (this will emit the prologue)
    builder.begin_function(local_slots);

    // Bind function parameters
    bind_function_parameters(function, &mut builder, mgr, naming);
    
    // Generate block labels
    let block_labels = generate_block_labels(function, naming);
    
    // Generate a common epilogue label
    let epilogue_label = naming.block_label(&function.name, 99999); // Use a high ID for epilogue
    
    // Track return instructions
    let mut has_any_return = false;
    let mut all_return_values = Vec::new();
    
    let vararg_fp_offset = if function.is_vararg {
        Some(FunctionBuilder::first_vararg_fp_offset(&function.parameters))
    } else {
        None
    };
    
    // Lower each basic block
    for (block_idx, block) in function.blocks.iter().enumerate() {
        let return_values = lower_basic_block(
            block,
            block_idx,
            function,
            &mut builder,
            mgr,
            naming,
            &block_labels,
            &alloca_offsets,
            global_manager,
            &epilogue_label,
            bank_size,
            vararg_fp_offset,
        )?;
        
        if !return_values.is_empty() {
            has_any_return = true;
            all_return_values.extend(return_values);
        }
    }
    
    // Generate the epilogue
    generate_epilogue(&mut builder, has_any_return, &all_return_values, epilogue_label);
    
    // Build and return the final instruction list
    let mut builder_instructions = builder.build();
    instructions.append(&mut builder_instructions);
    Ok(instructions)
}