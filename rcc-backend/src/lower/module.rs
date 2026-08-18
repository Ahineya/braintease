//! Module Lowering - Handles lowering of entire modules
//! 
//! This module is responsible for lowering IR modules to assembly,
//! including global variable initialization and function lowering.

use rcc_frontend::ir::Module;
use rcc_codegen::AsmInst;
use log::{debug, info};
use crate::globals::GlobalManager;
use crate::naming::new_function_naming;
use crate::regmgmt::RegisterPressureManager;
use super::function::lower_function_v2;

/// Lower an entire module using the V2 backend
pub fn lower_module(module: &Module, bank_size: u16, trace_spills: bool) -> Result<Vec<AsmInst>, String> {
    info!("V2: Lowering module '{}' with bank_size {}", module.name, bank_size);
    let mut all_instructions = Vec::new();
    
    // Create a global manager to handle global variable allocation
    let mut global_manager = GlobalManager::with_module(&module.name);
    
    // First pass: allocate addresses for all globals
    for global in &module.globals {
        global_manager.allocate_global(global);
    }

    // Reserve this TU's GP range in .data so the linker concatenates BSS
    // across separately compiled objects instead of every object starting at 0.
    all_instructions.extend(global_manager.emit_data_section());
    
    // Check if this module has a main function (indicating it's the main module)
    let has_main = module.functions.iter().any(|f| f.name == "main");
    
    // Only generate _init_globals for the main module
    // This avoids duplicate labels when linking multiple object files
    if has_main {
        all_instructions.push(AsmInst::Label("_init_globals".to_string()));
        
        // Generate initialization code for each global
        if !module.globals.is_empty() {
            info!("V2: Initializing {} globals", module.globals.len());
            
            for global in &module.globals {
                debug!("Processing global '{}' with initializer: {:?}", global.name, global.initializer);
                if let Some(info) = global_manager.get_global_info(&global.name) {
                    let global_insts = global_manager.lower_global_init(global, info);
                    all_instructions.extend(global_insts);
                }
            }
        }
        
        all_instructions.push(AsmInst::Ret);
    } else if !module.globals.is_empty() {
        // For library modules, still allocate space but don't generate init code
        info!("V2: Library module with {} globals (no _init_globals generated)", module.globals.len());
        
        // We still need to generate comments for globals so they can be referenced
        for global in &module.globals {
            if let Some(info) = global_manager.get_global_info(&global.name) {
                all_instructions.push(AsmInst::Comment(format!("Global '{}' at address {}", 
                                                               global.name, info.address)));
            }
            // The actual initialization will be done by the main module's _init_globals
        }
    }
    
    // Lower each function
    for function in &module.functions {
        if function.is_external {
            debug!("V2: Skipping external function '{}'", function.name);
            continue;
        }
        
        debug!("V2: Lowering function '{}'", function.name);
        
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
    
    info!("V2: Module lowering complete, generated {} instructions", all_instructions.len());
    Ok(all_instructions)
}