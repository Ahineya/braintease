//! Module Lowering - Handles lowering of entire modules
//! 
//! This module is responsible for lowering IR modules to assembly,
//! including global variable initialization and function lowering.

use rcc_frontend::ir::{Module, Linkage, Instruction, Value};
use rcc_codegen::AsmInst;
use log::{debug, info};
use crate::globals::GlobalManager;
use crate::naming::new_function_naming;
use crate::regmgmt::RegisterPressureManager;
use super::function::lower_function_v2;

/// Lower an entire module using the V2 backend
pub fn lower_module(module: &Module, bank_size: u16, trace_spills: bool) -> Result<Vec<AsmInst>, String> {
    let mut optimized = module.clone();
    for function in &mut optimized.functions {
        crate::optimize::optimize_function(function);
    }
    let module = &optimized;
    info!("V2: Lowering module '{}' with bank_size {}", module.name, bank_size);
    let mut all_instructions = Vec::new();
    
    // Create a global manager to handle global variable allocation
    let mut global_manager = GlobalManager::with_module_and_bank_size(&module.name, bank_size);
    
    // First pass: allocate addresses for all globals
    let total_words: u64 = module.globals.iter()
        .filter(|g| g.linkage != Linkage::Declaration)
        .map(|g| g.var_type.size_in_words().unwrap_or(1)).sum();
    if total_words > bank_size as u64 {
        return Err(format!("global storage needs {total_words} cells, exceeds one data bank ({bank_size}); split or dynamically allocate these objects"));
    }
    for global in &module.globals {
        global_manager.allocate_global(global);
    }

    // Reserve this TU's GP range in .data so the linker concatenates BSS
    // across separately compiled objects instead of every object starting at 0.
    all_instructions.extend(global_manager.emit_data_section());
    
    // Each object contributes an initializer; the linker constructs the dispatcher.
    if module.globals.iter().any(|g| g.linkage != Linkage::Declaration) {
        all_instructions.push(AsmInst::Label(format!("__rcc_init_{}", global_manager.unit_tag())));
        for global in &module.globals {
            if global.linkage == Linkage::Declaration { continue; }
            if let Some(info) = global_manager.get_global_info(&global.name) {
                all_instructions.extend(global_manager.lower_global_init(global, info));
            }
        }
        all_instructions.push(AsmInst::Ret);
    }

    // Lower each function
    for function in &module.functions {
        if function.is_external {
            debug!("V2: Skipping external function '{}'", function.name);
            continue;
        }
        
        debug!("V2: Lowering function '{}'", function.name);
        
        // The frame allocator uses signed 16-bit FP offsets. Validate before
        // narrowing sizes or adding offsets, rather than wrapping large arrays.
        let mut local_cells = 0u64;
        for instruction in function.blocks.iter().flat_map(|block| &block.instructions) {
            if let Instruction::Alloca { alloc_type, count, .. } = instruction {
                let elements = match count {
                    Some(Value::Constant(n)) => u64::try_from(*n)
                        .map_err(|_| format!("function '{}' has a negative local allocation size", function.name))?,
                    _ => 1,
                };
                let cells = alloc_type.size_in_words().unwrap_or(1).checked_mul(elements)
                    .ok_or_else(|| format!("function '{}' local allocation size overflow", function.name))?;
                local_cells = local_cells.checked_add(cells)
                    .ok_or_else(|| format!("function '{}' local allocation size overflow", function.name))?;
            }
        }
        // Eight local scratch cells, twenty initial spill cells, seven saved
        // registers; SP starts at one. Later lowering checks actual spill use.
        let minimum_frame = local_cells.checked_add(8 + 20 + 7)
            .ok_or_else(|| format!("function '{}' local frame size overflow", function.name))?;
        let frame_limit = u64::from(bank_size.saturating_sub(1)).min(i16::MAX as u64);
        if minimum_frame > frame_limit {
            return Err(format!("function '{}' local frame needs at least {minimum_frame} cells, exceeds stack frame capacity {frame_limit}", function.name));
        }

        // Calculate the number of local slots needed for this function
        // This is critical for proper spill slot allocation - the RegisterPressureManager
        // needs to know where locals end so it can place spill slots after them (FP + local_slots + spill_index)
        let local_slots = super::function::calculate_local_slots(function);
        debug!("V2: Function '{}' needs {} local slots", function.name, local_slots);
        
        // Create a fresh register manager and naming context for this function
        // The manager stores local_slots internally and lower_function_v2 will retrieve it
        let mut mgr = RegisterPressureManager::new(local_slots);
        mgr.set_trace_spills(trace_spills);
        mgr.set_gp_base_label(global_manager.gp_base_label());
        let mut naming = new_function_naming();
        
        let function_asm = lower_function_v2(function, &mut mgr, &mut naming, &global_manager, bank_size)?;
        all_instructions.extend(function_asm);
    }
    
    // Internal C linkage is object-local even when different files use the same names.
    let mut internal = module.internal_functions.clone();
    internal.extend(module.globals.iter().filter(|g| matches!(g.linkage, Linkage::Internal | Linkage::Private)).map(|g| g.name.clone()));
    for inst in &mut all_instructions {
        let label = match inst {
            AsmInst::Label(s) | AsmInst::Call(s) | AsmInst::LiLabel(_, s)
            | AsmInst::Beq(_, _, s) | AsmInst::Bne(_, _, s)
            | AsmInst::Blt(_, _, s) | AsmInst::Bge(_, _, s) => s,
            _ => continue,
        };
        for name in &internal {
            let unique = format!("__rcc_local_{}_{}", global_manager.unit_tag(), name);
            if *label == *name { *label = unique; break; }
            if *label == format!("%offset({name})") { *label = format!("%offset({unique})"); break; }
            if *label == format!("%bank({name})") { *label = format!("%bank({unique})"); break; }
            let block_prefix = format!("L_{name}_");
            if let Some(tail) = label.strip_prefix(&block_prefix) {
                *label = format!("L_{unique}_{tail}"); break;
            }
        }
    }
    info!("V2: Module lowering complete, generated {} instructions", all_instructions.len());
    Ok(all_instructions)
}
