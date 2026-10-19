//! Module Lowering - Handles lowering of entire modules
//! 
//! This module is responsible for lowering IR modules to assembly,
//! including global variable initialization and function lowering.

use rcc_ir::ir::{Module, Linkage, Instruction, Value};
use rcc_codegen::AsmInst;
use log::{debug, info};
use crate::globals::GlobalManager;
use crate::naming::new_function_naming;
use crate::regmgmt::RegisterPressureManager;
use super::function::lower_function_v2;
use std::collections::HashMap;

/// Lower an entire module using the V2 backend
pub fn lower_module(module: &Module, bank_size: u16, trace_spills: bool) -> Result<Vec<AsmInst>, String> {
    lower_module_with_stack_mode(module, bank_size, trace_spills, false)
}

pub fn lower_module_with_stack_mode(module: &Module, bank_size: u16, trace_spills: bool, segmented_stack: bool) -> Result<Vec<AsmInst>, String> {
    lower_module_with_memory_mode(module, bank_size, trace_spills, segmented_stack, true)
}

pub fn lower_module_with_memory_mode(module: &Module, bank_size: u16, trace_spills: bool, segmented_stack: bool, banked_globals: bool) -> Result<Vec<AsmInst>, String> {
    if module.target != rcc_ir::target::Target::Ripple {
        return Err("native IR cannot be lowered to Ripple; recompile source for --target ripple".into());
    }
    let mut optimized = module.clone();
    rcc_ir::float::lower_to_softfloat(&mut optimized)?;
    crate::optimize::optimize_module(&mut optimized);
    for function in &mut optimized.functions {
        crate::optimize::preserve_pointer_banks(function, bank_size);
        // Use ordering guards for bank proofs before rotating loops, then
        // keep the new private counter home through ordinary Phi destruction.
        crate::optimize::rotate_counted_loops(function);
        crate::optimize::destroy_phis(function)?;
        crate::optimize::cfg::order_for_lowering(function);
    }
    let module = &optimized;
    info!("V2: Lowering module '{}' with bank_size {}", module.name, bank_size);
    let mut all_instructions = Vec::new();
    let mut link_symbols = std::collections::BTreeMap::<String, Vec<String>>::new();
    let mut link_code = Vec::new();
    let mut link_roots = Vec::new();
    
    // Create a global manager to handle global variable allocation
    // First pass: allocate addresses for all globals
    let total_words: u64 = module.globals.iter()
        .filter(|g| g.linkage != Linkage::Declaration)
        .try_fold(0u64, |total, global| total.checked_add(global.var_type.size_in_words().unwrap_or(1)))
        .ok_or("global storage size overflow")?;
    let banked_globals = banked_globals || total_words.saturating_add(2) > u64::from(bank_size);
    if total_words > u64::from(bank_size) * 16 {
        return Err(format!("translation unit global storage needs {total_words} cells, exceeds 16 data banks"));
    }
    let mut global_manager = GlobalManager::with_module_and_bank_size(&module.name, bank_size).with_banked_globals(banked_globals);
    for global in &module.globals {
        global_manager.allocate_global(global);
    }

    // Reserve this TU's GP range in .data so the linker concatenates BSS
    // across separately compiled objects instead of every object starting at 0.
    all_instructions.extend(global_manager.emit_data_section());
    
    // Independent initializer entries let the linker retain exactly the globals
    // referenced by reachable functions (including pointers to other globals).
    // Keep the TU marker so old tools still recognize its GP data reservation.
    if module.globals.iter().any(|g| g.linkage != Linkage::Declaration) {
        let marker = format!("__rcc_init_{}", global_manager.unit_tag());
        all_instructions.push(AsmInst::Label(marker.clone()));
        all_instructions.push(AsmInst::Ret);
        link_code.push(marker.clone());
        link_roots.push(marker);
        for (index, global) in module.globals.iter().enumerate() {
            if global.linkage == Linkage::Declaration { continue; }
            let init = format!("__rcc_init_{}_g{index}", global_manager.unit_tag());
            link_code.push(init.clone());
            let mut references = std::collections::BTreeSet::new();
            if let Some(value) = &global.initializer {
                crate::optimize::value_symbols(value, &mut references);
            }
            link_symbols.insert(global.name.clone(), vec![init.clone()]);
            link_symbols.insert(init.clone(), references.into_iter().collect());
            all_instructions.push(AsmInst::Label(init));
            if let Some(info) = global_manager.get_global_info(&global.name) {
                all_instructions.extend(global_manager.lower_global_init(global, info));
            }
            all_instructions.push(AsmInst::Ret);
        }
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
        mgr.set_segmented_stack(segmented_stack);
        mgr.set_gp_base_label(global_manager.gp_base_label());
        let mut naming = new_function_naming();
        
        let function_asm = lower_function_v2(function, &mut mgr, &mut naming, &global_manager, bank_size)?;
        link_code.push(function.name.clone());
        let mut references = crate::optimize::function_symbols(function);
        if segmented_stack { references.insert("__rcc_stack_grow".into()); }
        if crate::optimize::function_has_opaque_asm(function) {
            // Opaque assembly can use GP offsets or hidden function addresses.
            // Retain this translation unit's symbols when such a body is live.
            references.extend(module.globals.iter().map(|g| g.name.clone()));
            references.extend(module.functions.iter().filter(|f| !f.is_external).map(|f| f.name.clone()));
        }
        link_symbols.insert(function.name.clone(), references.into_iter().collect());
        all_instructions.extend(function_asm);
    }
    
    // Internal C linkage is object-local even when different files use the same names.
    let mut internal = module.internal_functions.clone();
    internal.extend(module.globals.iter().filter(|g| matches!(g.linkage, Linkage::Internal | Linkage::Private)).map(|g| g.name.clone()));
    let renamer = InternalSymbolRenamer::new(&internal, global_manager.unit_tag());
    for inst in &mut all_instructions {
        let label = match inst {
            AsmInst::Label(s) | AsmInst::Call(s) | AsmInst::LiLabel(_, s)
            | AsmInst::Beq(_, _, s) | AsmInst::Bne(_, _, s)
            | AsmInst::Blt(_, _, s) | AsmInst::Bge(_, _, s) => s,
            _ => continue,
        };
        if let Some(unique) = renamer.assembly_label(label) { *label = unique; }
    }
    let rename = |name: &str| {
        renamer.symbols.get(name).cloned().unwrap_or_else(|| name.to_string())
    };
    let symbols: std::collections::BTreeMap<_, _> = link_symbols.into_iter()
        .map(|(name, refs)| (rename(&name), refs.iter().map(|n| rename(n)).collect::<Vec<_>>())).collect();
    let metadata = serde_json::json!({
        "banked_data": banked_globals,
        "symbols": symbols,
        "code": link_code.iter().map(|n| rename(n)).collect::<Vec<_>>(),
        "roots": link_roots,
        "data": module.globals.iter().filter(|g| g.linkage != Linkage::Declaration)
            .map(|g| (rename(&g.name), g.var_type.size_in_words().unwrap_or(1)))
            .collect::<std::collections::BTreeMap<_, _>>(),
    });
    all_instructions.insert(0, AsmInst::Comment(format!("rcc-link-v1 {metadata}")));
    info!("V2: Module lowering complete, generated {} instructions", all_instructions.len());
    Ok(all_instructions)
}

/// Build replacements once per translation unit. Block labels can contain
/// underscores in the function name, so checking only the first '_' is wrong.
struct InternalSymbolRenamer {
    symbols: HashMap<String, String>,
    exact: HashMap<String, (usize, String)>,
    blocks: HashMap<String, (usize, String)>,
}

impl InternalSymbolRenamer {
    fn new(names: &[String], unit: &str) -> Self {
        let mut this = Self { symbols: HashMap::new(), exact: HashMap::new(), blocks: HashMap::new() };
        for (index, name) in names.iter().enumerate() {
            let unique = format!("__rcc_local_{unit}_{name}");
            this.symbols.insert(name.clone(), unique.clone());
            for (from, to) in [
                (name.clone(), unique.clone()),
                (format!("%offset({name})"), format!("%offset({unique})")),
                (format!("%bank({name})"), format!("%bank({unique})")),
            ] {
                this.exact.entry(from).or_insert((index, to));
            }
            this.blocks.entry(name.clone()).or_insert((index, format!("L_{unique}_")));
        }
        this
    }

    fn assembly_label(&self, label: &str) -> Option<String> {
        for projection in ["%data_offset(", "%data_bank("] {
            if let Some(expression) = label.strip_prefix(projection).and_then(|s| s.strip_suffix(')')) {
                let (name, displacement) = expression.split_once('+').unwrap_or((expression, "0"));
                if let Some(unique) = self.symbols.get(name) {
                    return Some(format!("{projection}{unique}+{displacement})"));
                }
            }
        }
        let mut best = self.exact.get(label).map(|(index, value)| (*index, value.clone()));
        if let Some(block) = label.strip_prefix("L_") {
            for (offset, _) in block.match_indices('_') {
                if let Some((index, prefix)) = self.blocks.get(&block[..offset]) {
                    // Preserve the old first-match order for ambiguous names
                    // such as foo and foo_bar, including exact-label matches.
                    if best.as_ref().is_none_or(|(previous, _)| index < previous) {
                        best = Some((*index, format!("{prefix}{}", &block[offset + 1..])));
                    }
                }
            }
        }
        best.map(|(_, value)| value)
    }
}

#[cfg(test)]
mod tests {
    use super::InternalSymbolRenamer;

    #[test]
    fn internal_symbol_lookup_preserves_expressions_and_prefix_precedence() {
        // Compare against the original ordered implementation, including
        // overlapping function names and a C symbol resembling a block label.
        for names in [
            vec!["foo", "foo_bar", "L_foo_entry", "_hidden"],
            vec!["L_foo_entry", "foo_bar", "foo", "_hidden"],
        ] {
            let names: Vec<String> = names.into_iter().map(str::to_string).collect();
            let renamer = InternalSymbolRenamer::new(&names, "unit");
            for label in ["foo", "foo_bar", "%offset(foo)", "%bank(foo_bar)",
                "L_foo_entry", "L_foo_bar_loop_2", "L__hidden_done", "external",
                "%offset(external)", "foo_suffix", "L_foobar_entry"] {
                let mut expected = None;
                for name in &names {
                    let unique = format!("__rcc_local_unit_{name}");
                    if label == name { expected = Some(unique); break; }
                    if label == format!("%offset({name})") { expected = Some(format!("%offset({unique})")); break; }
                    if label == format!("%bank({name})") { expected = Some(format!("%bank({unique})")); break; }
                    if let Some(tail) = label.strip_prefix(&format!("L_{name}_")) {
                        expected = Some(format!("L_{unique}_{tail}")); break;
                    }
                }
                assert_eq!(renamer.assembly_label(label), expected, "{label}, {names:?}");
            }
        }
    }
}
