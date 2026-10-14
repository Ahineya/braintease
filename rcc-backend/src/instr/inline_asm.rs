//! Inline Assembly Support for V2 Backend
//! 
//! This module handles inline assembly statements with extended syntax:
//! asm("code" : outputs : inputs : clobbers)
//! 
//! The implementation follows GCC's extended inline assembly format with
//! proper register allocation, constraint parsing, and placeholder substitution.

use rcc_ir::ir::{AsmOperandIR, Value};
use rcc_codegen::{AsmInst, Reg};
use rcc_common::{CompilerError, SourceLocation};
use crate::naming::NameGenerator;
use log::{debug, warn};
use std::collections::{HashMap, BTreeSet};
use crate::regmgmt::RegisterPressureManager;

/// Parsed constraint information
#[derive(Debug, Clone)]
pub struct ParsedConstraint {
    /// Is this an output constraint (=), input/output (+), or input
    pub is_output: bool,
    /// Is this a read-write constraint (+)
    pub is_read_write: bool,
    /// The constraint type: 'r' for register, 'm' for memory, 'i' for immediate
    pub constraint_type: char,
    /// For read-write constraints, the index of the tied input operand
    pub tied_to: Option<usize>,
}

/// Operand with allocated register and constraint info
#[derive(Debug)]
struct AllocatedOperand {
    /// The original operand from IR
    operand: AsmOperandIR,
    /// Parsed constraint information
    constraint: ParsedConstraint,
    /// Allocated register for this operand
    register: Reg,
    /// Original value name (for tracking)
    value_name: String,
}

/// Parse a constraint string like "=r", "+r", "r", "m", "i"
fn parse_constraint(constraint_str: &str) -> ParsedConstraint {
    let mut chars = constraint_str.chars();
    let first = chars.next().unwrap_or('r');
    
    let (is_output, is_read_write, constraint_type) = match first {
        '=' => {
            // Output constraint
            let ctype = chars.next().unwrap_or('r');
            (true, false, ctype)
        }
        '+' => {
            // Input/output (read-write) constraint
            let ctype = chars.next().unwrap_or('r');
            (true, true, ctype)
        }
        _ => {
            // Input constraint (or immediate/memory)
            (false, false, first)
        }
    };
    
    ParsedConstraint {
        is_output,
        is_read_write,
        constraint_type,
        tied_to: None,
    }
}

/// Get a set of clobbered registers from string names
fn parse_clobbers(clobbers: &[String]) -> Result<BTreeSet<Reg>, CompilerError> {
    let mut clobbered = BTreeSet::new();
    
    for clobber in clobbers {
        // Use the built-in from_str method from ripple_asm::Register
        if matches!(clobber.as_str(), "memory" | "cc") {
            // Memory is already a barrier to the IR optimizer. Ripple has no
            // condition-code register; comparisons write ordinary registers.
            continue;
        }
        if let Some(reg) = Reg::from_str(&clobber.to_uppercase()) {
            if matches!(reg, Reg::R0 | Reg::Pc | Reg::Pcb | Reg::Sp | Reg::Fp | Reg::Sb | Reg::Gp) {
                return Err(asm_error(format!("Inline assembly cannot clobber reserved register {reg}")));
            }
            clobbered.insert(reg);
        } else {
            return Err(asm_error(format!("Unknown inline assembly clobber '{clobber}'")));
        }
    }
    
    Ok(clobbered)
}

fn asm_error(message: String) -> CompilerError {
    CompilerError::codegen_error(message, SourceLocation::new_simple(0, 0))
}

/// Allocate registers for operands based on constraints
fn allocate_operand_registers(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    outputs: &[AsmOperandIR],
    inputs: &[AsmOperandIR],
    _clobbers: &[String],
) -> Result<(Vec<AllocatedOperand>, Vec<AllocatedOperand>), CompilerError> {
    let mut allocated_outputs = Vec::new();
    let mut allocated_inputs = Vec::new();
    let mut register_map: HashMap<usize, Reg> = HashMap::new();
    
    // First pass: Process outputs and allocate registers for them
    for (idx, output) in outputs.iter().enumerate() {
        let constraint = parse_constraint(&output.constraint);
        
        // Validate constraint type
        if constraint.constraint_type != 'r' && constraint.constraint_type != 'm' {
            return Err(CompilerError::codegen_error(
                format!("Unsupported output constraint type: {}", constraint.constraint_type),
                SourceLocation::new_simple(0, 0),
            ));
        }
        
        // For register constraints, get a register from the manager
        if constraint.constraint_type == 'r' {
            // Get the value name for tracking
            let value_name = match &output.value {
                Value::Temp(t) => naming.temp_name(*t),
                _ => format!("asm_output_{idx}"),
            };
            
            // Get a register from the manager
            let reg = mgr.get_register(value_name.clone());
            mgr.pin_register(reg);
            register_map.insert(idx, reg);
            
            allocated_outputs.push(AllocatedOperand {
                operand: output.clone(),
                constraint: constraint.clone(),
                register: reg,
                value_name,
            });
        } else {
            // Memory constraint - not yet fully implemented
            return Err(CompilerError::codegen_error(
                "Memory constraints not yet implemented".to_string(),
                SourceLocation::new_simple(0, 0),
            ));
        }
    }
    
    // Second pass: Process inputs
    for (idx, input) in inputs.iter().enumerate() {
        // Check if this input is tied to an output
        let tied_output_idx = input.tied_to;
        
        if let Some(output_idx) = tied_output_idx {
            // This input is tied to an output, use the same register
            if output_idx >= allocated_outputs.len() {
                return Err(CompilerError::codegen_error(
                    format!("Invalid tied operand index: {output_idx}"),
                    SourceLocation::new_simple(0, 0),
                ));
            }
            
            let output_reg = allocated_outputs[output_idx].register;
            mgr.pin_register(output_reg);
            let value_name = match &input.value {
                Value::Temp(t) => naming.temp_name(*t),
                _ => format!("asm_input_{idx}"),
            };
            
            allocated_inputs.push(AllocatedOperand {
                operand: input.clone(),
                constraint: parse_constraint(&input.constraint),
                register: output_reg,
                value_name,
            });
        } else {
            // Regular input operand
            let constraint = parse_constraint(&input.constraint);
            
            if constraint.constraint_type == 'r' {
                let value_name = match &input.value {
                    Value::Temp(t) => naming.temp_name(*t),
                    Value::Constant(c) => naming.const_value(*c),
                    _ => format!("asm_input_{idx}"),
                };
                
                // Get a register from the manager
                let reg = mgr.get_register(value_name.clone());
                mgr.pin_register(reg);
                
                allocated_inputs.push(AllocatedOperand {
                    operand: input.clone(),
                    constraint,
                    register: reg,
                    value_name,
                });
            } else if constraint.constraint_type == 'i' {
                // Immediate constraint - these will be handled differently
                return Err(CompilerError::codegen_error(
                    "Immediate constraints not yet implemented".to_string(),
                    SourceLocation::new_simple(0, 0),
                ));
            } else if constraint.constraint_type == 'm' {
                // Memory constraint
                return Err(CompilerError::codegen_error(
                    "Memory constraints not yet implemented".to_string(),
                    SourceLocation::new_simple(0, 0),
                ));
            } else {
                return Err(CompilerError::codegen_error(
                    format!("Unsupported input constraint: {}", input.constraint),
                    SourceLocation::new_simple(0, 0),
                ));
            }
        }
    }
    
    Ok((allocated_outputs, allocated_inputs))
}

/// Substitute placeholders in assembly string with allocated registers
fn substitute_placeholders(
    assembly: &str,
    allocated_outputs: &[AllocatedOperand],
    allocated_inputs: &[AllocatedOperand],
) -> String {
    let mut result = assembly.to_string();
    
    // Highest numbered operands first: replacing %1 must not corrupt %10.
    let operands: Vec<_> = allocated_outputs.iter().chain(allocated_inputs).collect();
    for (idx, op) in operands.into_iter().enumerate().rev() {
        let placeholder = format!("%{idx}");
        let reg_name = format!("{:?}", op.register);
        result = result.replace(&placeholder, &reg_name);
    }
    
    result
}

/// Generate setup code to load input values into allocated registers
fn generate_setup_code(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    allocated_inputs: &[AllocatedOperand],
) -> Vec<AsmInst> {
    let mut insts = Vec::new();

    for op in allocated_inputs {
        let constraint = &op.constraint;
        if constraint.constraint_type != 'r' {
            // keep 'i'/'m' for later extension if you want
            continue;
        }

        match &op.operand.value {
            Value::Temp(t) => {
                let temp_name = naming.temp_name(*t);
                let src_reg = mgr.get_register(temp_name.clone());
                insts.extend(mgr.take_instructions());

                if src_reg != op.register {
                    insts.push(AsmInst::Comment(format!(
                        "Load input {} into {:?}", op.value_name, op.register
                    )));
                    insts.push(AsmInst::Move(op.register, src_reg));
                    mgr.bind_value_to_register(temp_name, op.register);
                }
                // An input already in its assigned register is unchanged.
                // Rebinding it would discard its still-valid spill home.
                // Do not free src_reg: the input may remain live afterwards.
            }
            Value::Constant(c) => {
                insts.push(AsmInst::Comment(format!(
                    "Load constant {} into {:?}", c, op.register
                )));
                insts.push(AsmInst::Li(op.register, *c as i16));
                // Bind the constant pseudo-name so the allocator knows
                let cname = naming.const_value(*c);
                mgr.bind_value_to_register(cname, op.register);
            }
            _ => {
                warn!("Unsupported input value type: {:?}", op.operand.value);
            }
        }
    }

    insts
}

/// Generate teardown code to store output values from registers
fn generate_teardown_code(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    allocated_outputs: &[AllocatedOperand],
) -> Result<Vec<AsmInst>, CompilerError> {
    let mut insts = Vec::new();

    for op in allocated_outputs {
        if op.constraint.constraint_type != 'r' {
            return Err(CompilerError::codegen_error(
                format!("Output constraint '{}' not yet supported", op.constraint.constraint_type),
                SourceLocation::new_simple(0, 0),
            ));
        }

        match &op.operand.value {
            Value::Temp(t) => {
                let temp_name = naming.temp_name(*t);
                // Tell the allocator: temp is in op.register now.
                mgr.bind_value_to_register(temp_name.clone(), op.register);
                insts.push(AsmInst::Comment(format!(
                    "Output {} now in {:?}", temp_name, op.register
                )));
                // No stores here. Normal codegen will use this binding.
            }
            other => {
                warn!("Unsupported output value target: {other:?}");
            }
        }
    }

    Ok(insts)
}

/// Lower extended inline assembly with operands
pub fn lower_inline_asm_extended(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    assembly: &str,
    outputs: &[AsmOperandIR],
    inputs: &[AsmOperandIR],
    clobbers: &[String],
) -> Result<Vec<AsmInst>, CompilerError> {
    debug!("Lowering extended inline assembly:");
    // Raw assembly may modify machine state outside its explicit operands.
    mgr.invalidate_resolved_banks();
    debug!("  Assembly: {assembly}");
    debug!("  Outputs: {outputs:?}");
    debug!("  Inputs: {inputs:?}");
    debug!("  Clobbers: {clobbers:?}");

    let mut insts = Vec::new();

    insts.push(AsmInst::Comment("=== Begin inline assembly ===".to_string()));
    let clobbered = parse_clobbers(clobbers)?;
    let allocatable = [Reg::T0, Reg::T1, Reg::T2, Reg::T3, Reg::T4, Reg::T5,
        Reg::T6, Reg::T7, Reg::S0, Reg::S1, Reg::S2, Reg::S3];
    let available = allocatable.iter().filter(|r| !clobbered.contains(r)).count();
    let required = outputs.len() + inputs.iter().filter(|op| op.tied_to.is_none()).count();
    if required > available {
        return Err(asm_error(format!("Inline assembly needs {required} operand registers but only {available} are available after clobbers")));
    }
    // Ordinary inputs stay in place. Only declared clobbers need spill homes;
    // reserving their physical registers also prevents operand overlap.
    for &register in &clobbered { mgr.reserve_clobber(register); }
    insts.extend(mgr.take_instructions());

    // Allocate regs for operands (keep only 'r' for now)
    let (allocated_outputs, allocated_inputs) =
        allocate_operand_registers(mgr, naming, outputs, inputs, clobbers)?;

    // Load/move inputs into their assigned regs
    insts.push(AsmInst::Comment("Setup: Load inputs".to_string()));
    insts.extend(generate_setup_code(mgr, naming, &allocated_inputs));
    insts.extend(mgr.take_instructions());

    // Substitute placeholders and emit raw asm
    let substituted = substitute_placeholders(assembly, &allocated_outputs, &allocated_inputs);
    insts.push(AsmInst::Comment("Inline assembly code".to_string()));
    for part in substituted.split([';', '\n']) {
        let t = part.trim();
            if !t.is_empty() { insts.push(structured_memory_instruction(t)); }
    }

    // Bind outputs to temps (no stores)
    insts.push(AsmInst::Comment("Teardown: Bind outputs".to_string()));
    insts.extend(generate_teardown_code(mgr, naming, &allocated_outputs)?);
    insts.extend(mgr.take_instructions());

    for operand in allocated_outputs.iter().chain(&allocated_inputs) {
        mgr.unpin_register(operand.register);
    }
    for register in clobbered { mgr.unpin_register(register); }

    mgr.invalidate_resolved_banks();
    insts.push(AsmInst::Comment("=== End inline assembly ===".to_string()));
    Ok(insts)
}

// Keep the memory operations used by runtime wrappers visible to frame
// analysis. Other assembly remains opaque and receives conservative saves.
fn structured_memory_instruction(text: &str) -> AsmInst {
    let words: Vec<_> = text.split(|c: char| c.is_whitespace() || c == ',')
        .filter(|s| !s.is_empty()).collect();
    if words.len() == 4 {
        if let (Some(a), Some(b), Some(c)) = (Reg::from_str(words[1]),
            Reg::from_str(words[2]), Reg::from_str(words[3])) {
            match words[0].to_uppercase().as_str() {
                "LOAD" => return AsmInst::Load(a, b, c),
                "STORE" => return AsmInst::Store(a, b, c),
                _ => {}
            }
        }
    }
    AsmInst::Raw(text.to_string())
}

/// Lower basic inline assembly (no operands)
pub fn lower_inline_asm_basic(assembly: &str) -> Vec<AsmInst> {
    let mut insts = Vec::new();
    
    // Split by semicolons or newlines to handle both styles
    for part in assembly.split([';', '\n']) {
        let trimmed = part.trim();
        if !trimmed.is_empty() {
            insts.push(AsmInst::Raw(trimmed.to_string()));
        }
    }
    
    insts
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::instr::tests::machine::Machine;

    fn input(id:u32) -> AsmOperandIR {
        AsmOperandIR {constraint:"r".into(),value:Value::Temp(id),tied_to:None}
    }

    #[test]
    fn store_operands_stay_in_registers_and_execute_exactly_once() {
        let mut mgr=RegisterPressureManager::new(0);
        mgr.init();
        let value=mgr.get_register("t1".into());
        let addr=mgr.get_register("t2".into());
        let code=lower_inline_asm_extended(&mut mgr,&mut NameGenerator::new(0),"STORE %0, R0, %1",&[],&[input(1),input(2)],&[]).unwrap();
        assert_eq!(code.iter().filter(|i| matches!(i,AsmInst::Store(..))).count(),1);
        assert!(!code.iter().any(|i| matches!(i,AsmInst::Load(..))));
        let mut machine=Machine::new(64000);
        machine.regs[value as usize]=0xf81f;
        machine.regs[addr as usize]=512;
        machine.run(&code);
        assert_eq!(machine.memory[&(0,512)],0xf81f);
    }

    #[test]
    fn unchanged_asm_inputs_keep_their_existing_spill_homes() {
        let mut mgr = RegisterPressureManager::new(0);
        mgr.init();
        mgr.get_register("t1".into());
        mgr.get_register("t2".into());
        mgr.spill_all();
        let homes = mgr.get_spill_count();
        mgr.take_instructions();
        let code = lower_inline_asm_extended(&mut mgr, &mut NameGenerator::new(0),
            "STORE %0, R0, %1", &[], &[input(1), input(2)], &[]).unwrap();
        assert_eq!(code.iter().filter(|i| matches!(i, AsmInst::Load(..))).count(), 2);
        mgr.spill_all();
        assert_eq!(mgr.get_spill_count(), homes);
        assert!(!mgr.take_instructions().iter().any(|i| matches!(i, AsmInst::Store(..))));
    }

    #[test]
    fn declared_clobber_is_saved_and_excluded_from_operands() {
        let mut mgr=RegisterPressureManager::new(0);
        mgr.init();
        let value=mgr.get_register("t1".into());
        assert_eq!(value,Reg::S3);
        let addr=mgr.get_register("t2".into());
        let code=lower_inline_asm_extended(&mut mgr,&mut NameGenerator::new(0),"STORE %0, R0, %1",&[],&[input(1),input(2)],&["s3".into()]).unwrap();
        assert!(code.iter().any(|i| matches!(i,AsmInst::Store(Reg::S3,Reg::Sb,Reg::Sc))));
        assert!(code.iter().any(|i| matches!(i,AsmInst::Store(src,Reg::R0,dst) if *src!=Reg::S3 && *dst!=Reg::S3)));
        let mut machine=Machine::new(64000);
        machine.regs[Reg::Fp as usize]=100;
        machine.regs[value as usize]=0xffff;
        machine.regs[addr as usize]=512;
        machine.run(&code);
        assert_eq!(machine.memory[&(0,512)],0xffff);
    }

    #[test]
    fn invalid_clobbers_fail_explicitly() {
        for clobber in ["bogus", "sp", "sb", "fp", "pc", "gp"] {
            assert!(parse_clobbers(&[clobber.into()]).is_err());
        }
        assert!(parse_clobbers(&["memory".into(),"cc".into(),"t0".into()]).is_ok());
    }
}
