//! IR Function Lowering - Converts IR functions to assembly
//! 
//! This is the main entry point for lowering complete IR functions to assembly.
//! It uses FunctionBuilder to handle all the complexity of function generation.
//! 
//! DO NOT CONFUSE WITH /v2/function/internal.rs which is an internal implementation detail.

use rcc_frontend::ir::{Function, Instruction, Value, IrType};
use rcc_frontend::BankTag;
use rcc_codegen::{AsmInst, Reg, emit_addr_constant};
use std::collections::HashMap;
use log::{debug, info, trace};
use crate::function::FunctionBuilder;
use crate::globals::GlobalManager;
use crate::instr::helpers::get_bank_register_with_runtime_check_safe;
use crate::naming::NameGenerator;
use crate::regmgmt::RegisterPressureManager;
use crate::regmgmt::liveness::FunctionLiveness;
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
    
    // Register spills are assigned separately and included in the final frame.
    slots
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
                    mgr.pin_register(bank_reg);
                    
                    let temp_reg = mgr.get_register(temp_name);
                    builder.add_instructions(mgr.take_instructions());
                    
                    // Move to return registers if needed
                    if temp_reg != Reg::Rv0 {
                        builder.add_instruction(AsmInst::Move(Reg::Rv0, temp_reg));
                    }
                    if bank_reg != Reg::Rv1 {
                        builder.add_instruction(AsmInst::Move(Reg::Rv1, bank_reg));
                    }
                    mgr.unpin_register(bank_reg);
                    trace!("  Returning fat pointer in Rv0/Rv1");
                    Some((Reg::Rv0, Some(Reg::Rv1)))
                } else if function.return_type.is_i64() || function.return_type.is_f64() {
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
                } else if function.return_type.is_two_word_scalar() {
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
                    } else if function.return_type.is_f32() {
                        builder.add_instruction(AsmInst::Li(Reg::Rv1, 0));
                    } else {
                        builder.add_instruction(AsmInst::Li(Reg::Sc, 15));
                        builder.add_instruction(AsmInst::Srl(Reg::Rv1, Reg::Rv0, Reg::Sc));
                        builder.add_instruction(AsmInst::Sub(Reg::Rv1, Reg::R0, Reg::Rv1));
                    }
                    trace!("  Returning two-word value in Rv0/Rv1");
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
                if function.return_type.is_i64() || function.return_type.is_f64() {
                    let parts = crate::instr::i64::split_const64(*c);
                    builder.add_instruction(AsmInst::Li(Reg::Rv0, parts[0]));
                    builder.add_instruction(AsmInst::Li(Reg::Rv1, parts[1]));
                    builder.add_instruction(AsmInst::Li(Reg::X0, parts[2]));
                    builder.add_instruction(AsmInst::Li(Reg::X1, parts[3]));
                    trace!("  Returning I64 constant {c} in Rv0/Rv1/X0/X1");
                    Some((Reg::Rv0, Some(Reg::Rv1)))
                } else if function.return_type.is_two_word_scalar() {
                    let (lo, hi) = crate::instr::wide::split_const(*c);
                    builder.add_instruction(AsmInst::Li(Reg::Rv0, lo));
                    builder.add_instruction(AsmInst::Li(Reg::Rv1, hi));
                    trace!("  Returning two-word constant {c} in Rv0/Rv1");
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
                        builder.add_instructions(emit_addr_constant(
                            Reg::Rv0,
                            *c as i16,
                            matches!(fp.bank, BankTag::Global),
                            mgr.gp_base_label(),
                        ));
                        Reg::Rv0
                    }
                    _ => panic!("Unexpected value type in FatPtr address: {:?}", fp.addr),
                };
                mgr.pin_register(addr_reg);
                
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
                mgr.unpin_register(addr_reg);
                
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
    liveness: &FunctionLiveness,
    preserve_incoming_registers: bool,
) -> Result<Vec<(Option<(Reg, Option<Reg>)>, usize)>, String> {
    debug!("Lowering block {} (index {}) [loop_condition={}]", block.id, block_idx, block.is_loop_condition);
    
    // A register assignment from the previous *emitted* block need not be
    // valid along the executed control-flow edge. Values are spilled before
    // branches; enter each successor with an empty physical-register cache.
    if block_idx != 0 && !preserve_incoming_registers {
        mgr.forget_all_registers();
    }
    if !preserve_incoming_registers { mgr.invalidate_alloca_bindings(); }
    builder.add_instructions(mgr.take_instructions());

    // Add label for the block (except for entry block which is implicit)
    if block.id != 0 {
        let label_name = block_labels.get(&block.id).unwrap().clone();
        builder.add_instruction(AsmInst::Label(label_name));
    }
    
    let mut return_values = Vec::new();
    
    // Lower each instruction in the block
    for (instruction_index, instruction) in block.instructions.iter().enumerate() {
        trace!("  Lowering instruction: {instruction:?}");
        mgr.set_live_after(&liveness.after[block_idx][instruction_index]);
        mgr.retain_live_registers(&liveness.before[block_idx][instruction_index]);
        
        match instruction {
            Instruction::Return(value) => {
                let value = value.as_ref().map(|v| crate::instr::helpers::canonicalize_value(v, global_manager, mgr, naming)).transpose()?;
                builder.add_instructions(mgr.take_instructions());
                let return_regs = handle_return_instruction(
                    &value,
                    function,
                    builder,
                    mgr,
                    naming,
                    epilogue_label,
                );
                return_values.push((return_regs, block_idx));
            }
            
            _ => {
                if matches!(instruction, Instruction::Branch(_)) {
                    mgr.spill_live_for_branch();
                    builder.add_instructions(mgr.take_instructions());
                }
                // Use the existing lower_instruction for other instructions
                let insts = lower_instruction(mgr, naming, instruction, &function.name, alloca_offsets, global_manager, bank_size, vararg_fp_offset)?;
                builder.add_instructions(insts);
            }
        }
        mgr.retain_live_registers(&liveness.after[block_idx][instruction_index]);
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
    let liveness = FunctionLiveness::analyze(function);
    // Legacy frontend goto blocks can contain unreachable instructions after
    // a terminator. They can still be emitted literally, but cannot authorize
    // CFG-based register reuse until the IR producer normalizes them.
    let cfg = crate::optimize::cfg::Cfg::new(function).ok();
    // Only a unique incoming edge from the immediately previous emitted block
    // authorizes register reuse. Joins, backedges and reordered blocks retain
    // the established stack-home contract.
    let preserve: Vec<_> = (0..function.blocks.len()).map(|i| {
        i > 0 && cfg.as_ref().is_some_and(|cfg| cfg.predecessors[i] == [i - 1]
            && cfg.successors[i - 1].contains(&i))
    }).collect();
    
    // Begin the function (this will emit the prologue)
    builder.begin_function(local_slots);

    if !function.blocks.iter().flat_map(|b| &b.instructions)
        .any(|i| matches!(i, Instruction::Call { .. })) {
        mgr.prefer_caller_saved();
    }

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
        let branch_spills = if preserve.get(block_idx + 1) == Some(&true) {
            // The following block retains bindings. Every other successor must
            // still find each live scalar, pointer bank and wide word at home.
            Some(cfg.as_ref().unwrap().successors[block_idx].iter().filter(|&&s| s != block_idx + 1)
                .flat_map(|&s| liveness.live_in[s].iter().copied())
                .collect())
        } else { None };
        mgr.set_branch_spill_roots(branch_spills);
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
            &liveness,
            preserve[block_idx],
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
    // Calls must use the final frame extent, including spills first allocated
    // after that call. Otherwise copying a returned aggregate can spill into
    // the just-returned callee's object before its contents have been copied.
    let frame_top = mgr.frame_high_water();
    if frame_top < 0 || frame_top as u32 + 8 >= bank_size as u32 {
        return Err(format!("function '{}' stack frame exceeds one stack bank ({bank_size} cells)", function.name));
    }
    let spill_reserve = frame_top - local_slots;
    let reserve_pos = builder_instructions.iter().position(|inst| {
        matches!(inst, AsmInst::Comment(s) if s == "Reserve 20 spill slots above locals")
    }).expect("function prologue must reserve spill storage");
    builder_instructions[reserve_pos] = AsmInst::Comment(format!("Reserve {spill_reserve} spill slots above locals"));
    builder_instructions[reserve_pos + 1] = AsmInst::AddI(Reg::Sp, Reg::Sp, spill_reserve);
    for index in 0..builder_instructions.len().saturating_sub(1) {
        if matches!(&builder_instructions[index], AsmInst::Comment(s) if s.starts_with("Set SP = FP+") && s.ends_with("so callee frame is above spills")) {
            builder_instructions[index] = AsmInst::Comment(format!("Set SP = FP+{frame_top} so callee frame is above spills"));
            builder_instructions[index + 1] = AsmInst::AddI(Reg::Sp, Reg::Fp, frame_top);
        }
    }
    instructions.extend(crate::function::frame::finalize(builder_instructions, frame_top));
    Ok(instructions)
}

#[cfg(test)]
mod lifetime_tests {
    use super::*;
    use rcc_frontend::ir::{BasicBlock, IrBinaryOp, IrUnaryOp};
    use crate::instr::tests::machine::Machine;

    fn lower_for_execution(function: &Function) -> (Vec<AsmInst>, RegisterPressureManager) {
        let mut manager = RegisterPressureManager::new(calculate_local_slots(function));
        manager.init();
        let mut assembly = lower_function_v2(function, &mut manager,
            &mut NameGenerator::new(0), &GlobalManager::new(), 64000).unwrap();
        // Falling off the test interpreter is equivalent to returning to its caller.
        assert!(matches!(assembly.pop(), Some(AsmInst::Jalr(..))));
        (assembly, manager)
    }

    fn execute(assembly: &[AsmInst], args: &[u16]) -> Machine {
        let mut machine = Machine::new(64000);
        machine.regs[Reg::Sp as usize] = 1000;
        machine.regs[Reg::Fp as usize] = 700;
        for (reg, arg) in [Reg::A0, Reg::A1, Reg::A2, Reg::A3].iter().zip(args) {
            machine.regs[*reg as usize] = *arg;
        }
        machine.run(assembly);
        assert_eq!(machine.regs[Reg::Sp as usize], 1000);
        assert_eq!(machine.regs[Reg::Fp as usize], 700);
        machine
    }

    fn block(id: u32, instructions: Vec<Instruction>) -> BasicBlock {
        let mut block = BasicBlock::new(id);
        block.instructions = instructions;
        block
    }

    #[test]
    fn dead_branch_predicate_needs_no_stack_round_trip() {
        let mut function = Function::new("dead_predicate".into(), IrType::I16);
        function.add_parameter(0, IrType::I16);
        function.blocks = vec![
            block(0, vec![Instruction::BranchCond {
                condition: Value::Temp(0), true_label: 1, false_label: 2,
            }]),
            block(1, vec![Instruction::Return(Some(Value::Constant(11)))]),
            block(2, vec![Instruction::Return(Some(Value::Constant(22)))]),
        ];
        let (assembly, manager) = lower_for_execution(&function);
        assert_eq!(manager.get_spill_count(), 0, "dead predicate must never be spilled");
        for (condition, expected) in [(0, 22), (1, 11), (32768, 11), (65535, 11)] {
            assert_eq!(execute(&assembly, &[condition]).regs[Reg::Rv0 as usize], expected);
        }
    }

    #[test]
    fn wide_branch_predicate_survives_both_successors_under_pressure() {
        for ty in [IrType::I32, IrType::I64] {
            let mut function = Function::new("wide_predicate".into(), ty.clone());
            function.add_parameter(0, ty.clone());
            let mut entry = Vec::new();
            // Every value crosses the branch; there are more live words than registers.
            for temp in 1..=20 {
                entry.push(Instruction::Binary { result: temp, op: IrBinaryOp::Add,
                    lhs: Value::Constant(temp as i64), rhs: Value::Constant(100),
                    result_type: IrType::I16 });
            }
            entry.push(Instruction::BranchCond {
                condition: Value::Temp(0), true_label: 1, false_label: 2,
            });
            function.blocks.push(block(0, entry));
            for arm in [1, 2] {
                let mut instructions = Vec::new();
                let mut previous = 0;
                for temp in 1..=20 {
                    let result = 100 * arm + temp;
                    instructions.push(Instruction::Binary { result, op: IrBinaryOp::Add,
                        lhs: Value::Temp(previous), rhs: Value::Temp(temp), result_type: ty.clone() });
                    previous = result;
                }
                // Distinguish paths even when all original predicate words are zero.
                instructions.push(Instruction::Binary { result: 300 + arm, op: IrBinaryOp::Add,
                    lhs: Value::Temp(previous), rhs: Value::Constant(arm as i64), result_type: ty.clone() });
                instructions.push(Instruction::Return(Some(Value::Temp(300 + arm))));
                function.blocks.push(block(arm, instructions));
            }
            let (assembly, manager) = lower_for_execution(&function);
            assert!(manager.get_spill_count() > 12);
            let values = if ty == IrType::I32 {
                vec![0u64, 1, 0x10000, 0x80000000, 0xffffffff]
            } else {
                vec![0u64, 1, 0x10000, 0x100000000, 0x8000000000000000, u64::MAX]
            };
            for value in values {
                let args: Vec<_> = (0..ty.abi_slots()).map(|word| (value >> (16 * word)) as u16).collect();
                let machine = execute(&assembly, &args);
                let expected = value.wrapping_add(2210 + if value == 0 { 2 } else { 1 });
                for (word, reg) in [Reg::Rv0, Reg::Rv1, Reg::X0, Reg::X1].iter().take(ty.abi_slots()).enumerate() {
                    assert_eq!(machine.regs[*reg as usize], (expected >> (16 * word)) as u16,
                        "{ty:?} input {value:#x}, word {word}");
                }
            }
        }
    }

    #[test]
    fn pointer_bank_survives_a_branch_and_alternative_successor_emission() {
        let pointer = IrType::FatPtr(Box::new(IrType::I16));
        let mut function = Function::new("pointer_predicate".into(), pointer.clone());
        function.add_parameter(0, pointer);
        // Pointer conditions have already been normalized by the frontend.
        function.add_parameter(1, IrType::I16);
        function.blocks = vec![
            block(0, vec![Instruction::BranchCond {
                condition: Value::Temp(1), true_label: 1, false_label: 2,
            }]),
            block(1, vec![Instruction::Branch(3)]),
            block(2, vec![Instruction::Branch(3)]),
            block(3, vec![Instruction::Return(Some(Value::Temp(0)))]),
        ];
        let (assembly, _) = lower_for_execution(&function);
        for addr in [0, 1, 63999] {
            for bank in [0, 1, 41] {
                for condition in [0, 1] {
                    let machine = execute(&assembly, &[addr, bank, condition]);
                    assert_eq!(machine.regs[Reg::Rv0 as usize], addr);
                    assert_eq!(machine.regs[Reg::Rv1 as usize], bank);
                }
            }
        }
    }

    #[test]
    fn normalized_pointer_truth_keeps_bank_only_nonnull_values() {
        let mut function = Function::new("pointer_truth".into(), IrType::I32);
        function.add_parameter(0, IrType::FatPtr(Box::new(IrType::I16)));
        function.blocks = vec![
            block(0, vec![
                Instruction::Unary { result: 1, op: IrUnaryOp::PtrToInt,
                    operand: Value::Temp(0), result_type: IrType::I32 },
                Instruction::Binary { result: 2, op: IrBinaryOp::Ne,
                    lhs: Value::Temp(1), rhs: Value::Constant(0), result_type: IrType::I16 },
                Instruction::BranchCond { condition: Value::Temp(2), true_label: 1, false_label: 2 },
            ]),
            block(1, vec![
                Instruction::Unary { result: 3, op: IrUnaryOp::PtrToInt,
                    operand: Value::Temp(0), result_type: IrType::I32 },
                Instruction::Return(Some(Value::Temp(3))),
            ]),
            block(2, vec![Instruction::Return(Some(Value::Constant(77)))]),
        ];
        let (assembly, _) = lower_for_execution(&function);
        for (addr, bank, expected) in [(0, 0, 77u32), (0, 41, 41 << 16), (63999, 41, (41 << 16) | 63999)] {
            let machine = execute(&assembly, &[addr, bank]);
            assert_eq!(machine.regs[Reg::Rv0 as usize], expected as u16);
            assert_eq!(machine.regs[Reg::Rv1 as usize], (expected >> 16) as u16);
        }
    }

    #[test]
    fn branch_predicate_reused_on_loop_backedges_is_preserved() {
        let mut function = Function::new("loop_predicate".into(), IrType::I16);
        function.add_parameter(0, IrType::I16);
        function.blocks = vec![
            block(0, vec![
                Instruction::Alloca { result: 1, alloc_type: IrType::I16, count: None,
                    result_type: IrType::FatPtr(Box::new(IrType::I16)) },
                Instruction::Store { value: Value::Temp(0), ptr: Value::Temp(1),
                    value_type: IrType::I16, volatile: false },
                Instruction::Branch(1),
            ]),
            block(1, vec![
                Instruction::Load { result: 2, ptr: Value::Temp(1), result_type: IrType::I16, volatile: false },
                Instruction::BranchCond { condition: Value::Temp(2), true_label: 2, false_label: 3 },
            ]),
            block(2, vec![
                Instruction::Binary { result: 3, op: IrBinaryOp::Sub,
                    lhs: Value::Temp(2), rhs: Value::Constant(1), result_type: IrType::I16 },
                Instruction::Store { value: Value::Temp(3), ptr: Value::Temp(1),
                    value_type: IrType::I16, volatile: false },
                Instruction::Branch(1),
            ]),
            block(3, vec![Instruction::Return(Some(Value::Temp(2)))]),
        ];
        let (assembly, _) = lower_for_execution(&function);
        for count in [0, 1, 2, 19] {
            assert_eq!(execute(&assembly, &[count]).regs[Reg::Rv0 as usize], 0);
        }
    }

    #[test]
    fn unique_adjacent_successor_reuses_registers_without_spilling() {
        let mut function = Function::new("adjacent_edge".into(), IrType::I16);
        function.add_parameter(0, IrType::I16);
        function.add_parameter(1, IrType::I16);
        function.blocks = vec![
            block(0, vec![Instruction::BranchCond { condition: Value::Temp(0), true_label: 1, false_label: 2 }]),
            block(1, vec![Instruction::Binary { result: 2, op: IrBinaryOp::Add,
                lhs: Value::Temp(1), rhs: Value::Temp(0), result_type: IrType::I16 },
                Instruction::Return(Some(Value::Temp(2)))]),
            block(2, vec![Instruction::Return(Some(Value::Constant(77)))]),
        ];
        let (assembly, manager) = lower_for_execution(&function);
        assert_eq!(manager.get_spill_count(), 0, "sole incoming edge keeps both value and predicate registers");
        for condition in [0, 1, 9, 65535] {
            assert_eq!(execute(&assembly, &[condition, 123]).regs[Reg::Rv0 as usize],
                if condition == 0 { 77 } else { 123u16.wrapping_add(condition) });
        }
    }

    #[test]
    fn adjacent_edge_spills_values_needed_through_an_empty_alternative() {
        let mut function = Function::new("empty_alternative".into(), IrType::I32);
        function.add_parameter(0, IrType::I32);
        function.add_parameter(1, IrType::I16);
        function.blocks = vec![
            block(0, vec![Instruction::BranchCond { condition: Value::Temp(1), true_label: 1, false_label: 2 }]),
            block(1, vec![Instruction::Binary { result: 2, op: IrBinaryOp::Add,
                lhs: Value::Temp(0), rhs: Value::Constant(65537), result_type: IrType::I32 },
                Instruction::Return(Some(Value::Temp(2)))]),
            block(2, vec![]),
            block(3, vec![Instruction::Return(Some(Value::Temp(0)))]),
        ];
        let (assembly, manager) = lower_for_execution(&function);
        assert!(manager.get_spill_count() >= 2, "both words need homes before taking the alternative");
        for value in [0u32, 0x1234ffff, u32::MAX] {
            for condition in [0, 1] {
                let result = execute(&assembly, &[value as u16, (value >> 16) as u16, condition]);
                let expected = if condition == 0 { value } else { value.wrapping_add(65537) };
                assert_eq!(result.regs[Reg::Rv0 as usize], expected as u16);
                assert_eq!(result.regs[Reg::Rv1 as usize], (expected >> 16) as u16);
            }
        }
    }

    #[test]
    fn lowering_reclaims_dead_ir_results_before_they_fill_registers() {
        let mut function = Function::new("discard_dead_results".into(), IrType::I16);
        let mut block = BasicBlock::new(0);
        for result in 0..100 {
            block.instructions.push(Instruction::Binary {
                result,
                op: IrBinaryOp::Add,
                lhs: Value::Constant(result as i64),
                rhs: Value::Constant(5),
                result_type: IrType::I16,
            });
        }
        block.instructions.push(Instruction::Return(Some(Value::Temp(99))));
        function.blocks.push(block);
        let mut manager = RegisterPressureManager::new(8);
        manager.init();
        let assembly = lower_function_v2(&function, &mut manager,
            &mut NameGenerator::new(0), &GlobalManager::new(), 64000).unwrap();
        assert_eq!(manager.get_spill_count(), 0);
        assert!(!assembly.iter().any(|instruction| {
            matches!(instruction, AsmInst::Comment(text) if text.starts_with("Spill "))
        }));
    }
}
