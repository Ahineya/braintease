//! GetElementPtr (GEP) instruction lowering for V2 backend
//!
//! Handles pointer arithmetic with proper bank overflow handling.
//! GEP is critical for array/struct access and must correctly handle
//! crossing bank boundaries (4096 instruction / 64000 byte boundaries).

use super::helpers::resolve_bank_tag_to_info;
use crate::naming::NameGenerator;
use crate::regmgmt::{BankInfo, RegisterPressureManager};
use log::{debug, trace, warn};
use rcc_codegen::{AsmInst, Reg};
use rcc_common::TempId;
use rcc_frontend::ir::Value;
// Use the bank size constant from V2 module
// TODO: This should eventually be passed as a parameter from rcc-driver

/// Lower a GetElementPtr instruction to assembly
///
/// This function handles pointer arithmetic with bank overflow detection.
/// When a pointer offset crosses a bank boundary, both the address and bank
/// must be updated correctly.
///
/// # CRITICAL: Array Indexing vs Struct Field Access
/// 
/// This function handles BOTH array indexing and struct field offsets, but they
/// work differently:
/// 
/// - **Array Indexing**: `indices[0]` is an array index that needs to be multiplied
///   by `element_size` to get the offset in words. Example: `arr[5]` with 2-word
///   elements gives offset = 5 * 2 = 10 words.
///
/// - **Struct Field Access**: `indices[0]` is ALREADY the offset in words! The frontend
///   has already calculated the field offset. We should NOT multiply it by element_size.
///   Example: accessing field at offset 1 should use offset = 1, not 1 * field_size.
///
/// The caller (instruction.rs) must set `element_size = 1` for struct field access
/// to prevent unwanted multiplication.
///
/// # Bank Overflow Formula
/// ```text
/// total_addr = base_addr + (index * element_size_in_cells)
/// new_bank = base_bank + (total_addr / 4096)
/// new_addr = total_addr % 4096
/// ```
///
/// # Parameters
/// - `mgr`: Register pressure manager for allocation and spilling
/// - `naming`: Name generator for unique temporary names
/// - `base_ptr`: Base pointer value (must be a FatPtr with bank info)
/// - `indices`: Array of indices to apply (typically just one for simple arrays)
/// - `element_size`: Size of each element in cells (16-bit words)
///                  MUST BE 1 for struct field access!
/// - `result_temp`: Temp ID for the result pointer
///
/// # Returns
/// Vector of assembly instructions for the GEP operation
pub fn lower_gep(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    base_ptr: &Value,
    indices: &[Value],
    element_size: i16,
    result_temp: TempId,
    bank_size: u16,
) -> Vec<AsmInst> {
    debug!(
        "lower_gep: base={base_ptr:?}, indices={indices:?}, elem_size={element_size}, result=t{result_temp}"
    );
    trace!("  Current register state: {:?}", mgr.get_spill_count());

    let mut insts = vec![];
    let result_name = naming.temp_name(result_temp);

    // Step 1: Extract base pointer components (address and bank)
    let (base_addr_reg, _base_ptr_name, base_bank_info) = match base_ptr {
        Value::Temp(t) => {
            let name = naming.temp_name(*t);
            trace!("  Base pointer is temp: {name}");
            let addr_reg = mgr.get_register(name.clone());
            let bank_info = mgr.get_pointer_bank(&name)
                .unwrap_or_else(|| {
                    panic!("GEP: COMPILER BUG: No bank info for pointer '{name}'. All pointers must have tracked bank information!");
                });
            (addr_reg, name, bank_info)
        }
        Value::FatPtr(fp) => {
            trace!("  Base pointer is fat pointer with bank {:?}", fp.bank);

            // Get address register
            let addr_reg = match fp.addr.as_ref() {
                Value::Temp(t) => {
                    let name = naming.temp_name(*t);
                    mgr.get_register(name)
                }
                Value::Constant(c) => {
                    // Load constant into register
                    let temp_name = naming.gep_offset_temp(result_temp);
                    let temp_reg = mgr.get_register(temp_name);
                    insts.push(AsmInst::Li(temp_reg, *c as i16));
                    trace!("  Loaded constant base address {c} into {temp_reg:?}");
                    temp_reg
                }
                _ => {
                    warn!("  Invalid address type in fat pointer: {:?}", fp.addr);
                    panic!("Invalid fat pointer address type in GEP")
                }
            };

            // Get bank info
            let bank_info = resolve_bank_tag_to_info(&fp.bank, fp, mgr, naming);

            let ptr_name = naming.pointer_bank_key(&result_name);
            (addr_reg, ptr_name, bank_info)
        }
        Value::Global(name) => {
            // This should never happen — lower.rs must resolve globals to FatPtr(Constant(addr), Global)
            panic!(
                "Unexpected Value::Global('{name}') as base pointer for GEP - should have been resolved to FatPtr(Constant(addr), Global) in lower.rs"
            );
        }
        _ => {
            warn!("  Invalid base pointer for GEP: {base_ptr:?}");
            panic!("Invalid base pointer type for GEP")
        }
    };

    debug!(
        "  Base address in {base_addr_reg:?}, bank info: {base_bank_info:?}"
    );

    // Take any instructions generated by register allocation
    insts.extend(mgr.take_instructions());

    // Step 2: Calculate total offset
    // For now, we support single index (array access)
    // TODO: Support multi-dimensional arrays and struct field access
    if indices.len() != 1 {
        warn!("  Multi-index GEP not yet fully supported, using first index only");
        panic!("Multi-index GEP not yet implemented");
    }

    let index = &indices[0];
    trace!("  Processing index: {index:?}");

    // Step 3: Check if we can determine offset statically
    let static_offset = match index {
        Value::Constant(idx) => {
            // IMPORTANT: This multiplication assumes array indexing!
            // For struct field access, the caller MUST pass element_size = 1
            // because idx is already the offset in words, not an array index.
            let offset = *idx as i16 * element_size;
            trace!(
                "  Static offset calculation: {idx} * {element_size} = {offset}"
            );
            Some(offset)
        }
        _ => None,
    };

    // Step 4: Generate offset calculation and bank overflow handling
    // Pin the base so allocating the result cannot reuse it, and flush any
    // spill/reload from get_register *before* Add/AddI. Otherwise the GEP
    // clobbers a live value (e.g. a later byval param) and the delayed spill
    // stores the already-overwritten register.
    mgr.pin_register(base_addr_reg);
    let result_addr_reg = mgr.get_register(result_name.clone());
    insts.extend(mgr.take_instructions());
    mgr.unpin_register(base_addr_reg);
    let mut result_bank_info = base_bank_info.clone();

    if let Some(offset) = static_offset {
        // Static offset - we can check for bank overflow at compile time
        debug!("  Static GEP offset: {offset}");

        // Add offset to base address
        if offset != 0 {
            insts.push(AsmInst::AddI(result_addr_reg, base_addr_reg, offset));
            trace!("  Added static offset {offset} to base address");
        } else {
            // No offset, just copy base address
            insts.push(AsmInst::Add(result_addr_reg, base_addr_reg, Reg::R0));
            trace!("  Zero offset, copying base address");
        }

        // Check for bank overflow (only needed if offset is large)
        if offset.abs() >= bank_size as i16 {
            let bank_crossing = offset / bank_size as i16;
            let new_offset = offset % bank_size as i16;

            warn!(
                "  Static bank overflow detected! Offset {offset} crosses {bank_crossing} banks"
            );
            debug!(
                "  Adjusting: bank_offset={bank_crossing}, addr_offset={new_offset}"
            );

            // Add warning comment to the generated assembly
            insts.push(AsmInst::Comment(format!(
                "WARNING: Static bank overflow - offset {offset} crosses {bank_crossing} banks"
            )));

            // For static bank info, we'd need to adjust the bank
            // This is complex and depends on the architecture
            match result_bank_info {
                BankInfo::Global => {
                    // Global bank doesn't change (uses GP register)
                    trace!("  Global bank remains using GP register");
                }
                BankInfo::Stack => {
                    if bank_crossing != 0 {
                        // Stack bank overflow - this is an error in most cases
                        warn!(
                            "  Stack bank overflow by {bank_crossing} banks - may be invalid!"
                        );
                    }
                }
                BankInfo::Heap(bank) => {
                    if bank_crossing != 0 {
                        // Heap bank crossing - update to new bank
                        let new_bank = (bank as i16 + bank_crossing) as u16;
                        result_bank_info = BankInfo::Heap(new_bank);
                        debug!("  Updated heap bank from {} to {}", bank, new_bank);
                    }
                }
                BankInfo::Dynamic(_) => {
                    // Named values track their bank dynamically
                    trace!("  Bank is tracked via named value");
                }
                BankInfo::Register(bank_reg) => {
                    // Dynamic bank - need to add bank offset
                    if bank_crossing != 0 {
                        let temp_bank_name = naming.gep_bank_temp(result_temp);
                        mgr.pin_register(bank_reg);
                        let new_bank_reg = mgr.get_register(temp_bank_name);
                        insts.extend(mgr.take_instructions());
                        mgr.unpin_register(bank_reg);
                        insts.push(AsmInst::AddI(new_bank_reg, bank_reg, bank_crossing));
                        result_bank_info = BankInfo::Register(new_bank_reg);
                        debug!("  Updated dynamic bank register by {bank_crossing}");
                    }
                }
            }
        }
    } else {
        // Dynamic offset - need runtime calculation
        debug!("  Dynamic GEP offset calculation required");

        // Get index register
        let index_reg = match index {
            Value::Temp(t) => {
                let name = naming.temp_name(*t);
                mgr.get_register(name)
            }
            _ => {
                warn!("  Unexpected dynamic index type: {index:?}");
                panic!("Invalid dynamic index type")
            }
        };

        trace!("  Index in register {index_reg:?}");
        insts.extend(mgr.take_instructions());

        // Calculate offset = index * element_size
        let offset_name = naming.gep_offset_temp(result_temp);
        let offset_reg = mgr.get_register(offset_name);
        insts.extend(mgr.take_instructions());

        if element_size == 1 {
            // No multiplication needed
            insts.push(AsmInst::Add(offset_reg, index_reg, Reg::R0));
            trace!("  Element size is 1, using index directly");
        } else if element_size > 0 && (element_size & (element_size - 1)) == 0 {
            // Use shift for power-of-2 sizes
            let shift_amount = element_size.trailing_zeros() as i16;
            let shift_reg = mgr.get_register(naming.gep_shift(result_temp));
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(shift_reg, shift_amount));
            insts.push(AsmInst::Sll(offset_reg, index_reg, shift_reg));
            mgr.free_register(shift_reg);
            trace!(
                "  Using shift by {shift_amount} for element size {element_size}"
            );
        } else {
            // Need multiplication
            let size_reg = mgr.get_register(naming.gep_size(result_temp));
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(size_reg, element_size));
            insts.push(AsmInst::Mul(offset_reg, index_reg, size_reg));
            mgr.free_register(size_reg);
            trace!("  Using multiplication for element size {element_size}");
        }

        // Add offset to base address
        insts.push(AsmInst::Add(result_addr_reg, base_addr_reg, offset_reg));
        debug!("  Added dynamic offset to base address");

        // All pointer types may cross banks, need overflow handling
        insts.push(AsmInst::Comment(
            "Runtime bank overflow calculation for dynamic GEP".to_string(),
        ));

        // Calculate how many banks we've crossed: bank_delta = result_addr / BANK_SIZE
        let bank_delta_reg = mgr.get_register(naming.gep_bank_delta(result_temp));
        let bank_size_reg = mgr.get_register(naming.gep_bank_size(result_temp));
        insts.extend(mgr.take_instructions());

        insts.push(AsmInst::Li(bank_size_reg, bank_size as i16));
        insts.push(AsmInst::Div(bank_delta_reg, result_addr_reg, bank_size_reg));
        trace!("  Calculated bank delta (banks crossed)");

        // Calculate address within new bank: new_addr = result_addr % BANK_SIZE
        let new_addr_reg = mgr.get_register(naming.gep_new_addr(result_temp));
        insts.extend(mgr.take_instructions());
        insts.push(AsmInst::Mod(new_addr_reg, result_addr_reg, bank_size_reg));
        trace!("  Calculated address within new bank");

        // Now update the bank based on the original bank info
        insts.push(AsmInst::Comment(format!("Base bank info: {base_bank_info:?}")));
        match base_bank_info {
            BankInfo::Dynamic(name) => {
                let new_bank_name = naming.gep_new_bank(result_temp);
                
                // CRITICAL FIX: Always clear the old binding first to ensure fresh computation
                // This prevents using stale values from previous iterations
                mgr.clear_value_binding(&new_bank_name);
                insts.extend(mgr.take_instructions());
                
                // Get the current bank register for the named value
                // This will reload from spill slot if necessary
                let current_bank = mgr.get_register(name.clone());
                insts.extend(mgr.take_instructions());
                
                // Always allocate a fresh register for the new bank value
                let new_bank_reg = mgr.get_register(new_bank_name.clone());
                insts.extend(mgr.take_instructions());
                
                insts.push(AsmInst::Comment(format!("Computing new bank {new_bank_name} = {name} + bank_delta")));
                insts.push(AsmInst::Add(new_bank_reg, current_bank, bank_delta_reg));
                
                // Bind the bank value to its register so it can be tracked/reloaded
                mgr.bind_value_to_register(new_bank_name.clone(), new_bank_reg);
                // CRITICAL: Use Dynamic so the bank can be reloaded if spilled
                result_bank_info = BankInfo::Dynamic(new_bank_name.clone());
                insts.push(AsmInst::Comment(format!("Result bank tracked as Dynamic({new_bank_name})")));
                debug!("  Updated named value pointer bank to trackable named value");
            }
            BankInfo::Global => {
                // Global bank: new_bank = GP + bank_delta
                let new_bank_name = naming.gep_new_bank(result_temp);
                
                // CRITICAL FIX: Clear any existing binding for this bank name
                mgr.clear_value_binding(&new_bank_name);
                insts.extend(mgr.take_instructions());  // Take clearing instructions
                
                let new_bank_reg = mgr.get_register(new_bank_name.clone());
                insts.extend(mgr.take_instructions());
                insts.push(AsmInst::Comment(format!("Computing new bank {new_bank_name} = GP + bank_delta")));
                insts.push(AsmInst::Add(new_bank_reg, Reg::Gp, bank_delta_reg));
                // Bind the bank value to its register so it can be tracked/reloaded
                mgr.bind_value_to_register(new_bank_name.clone(), new_bank_reg);
                // CRITICAL: Use Dynamic so the bank can be reloaded if spilled
                result_bank_info = BankInfo::Dynamic(new_bank_name.clone());
                insts.push(AsmInst::Comment(format!("Result bank tracked as Dynamic({new_bank_name})")));
                debug!("  Updated global-based pointer bank to trackable named value");
            }
            BankInfo::Register(existing_bank_reg) => {
                // Already dynamic: update in place
                insts.push(AsmInst::Add(
                    existing_bank_reg,
                    existing_bank_reg,
                    bank_delta_reg,
                ));
                debug!("  Updated existing dynamic bank register");
            }
            BankInfo::Stack => {
                // Stack bank: new_bank = SB + bank_delta  
                let new_bank_name = naming.gep_new_bank(result_temp);
                
                // CRITICAL FIX: Clear any existing binding for this bank name
                // This ensures we recompute the bank value in loops rather than
                // reusing a stale value from a previous iteration
                mgr.clear_value_binding(&new_bank_name);
                insts.extend(mgr.take_instructions());  // Take clearing instructions
                
                let new_bank_reg = mgr.get_register(new_bank_name.clone());
                insts.extend(mgr.take_instructions());
                insts.push(AsmInst::Comment(format!("Computing new bank {new_bank_name} = SB + bank_delta")));
                insts.push(AsmInst::Add(new_bank_reg, Reg::Sb, bank_delta_reg));
                // Bind the bank value to its register so it can be tracked/reloaded
                mgr.bind_value_to_register(new_bank_name.clone(), new_bank_reg);
                // CRITICAL: Use Dynamic so the bank can be reloaded if spilled
                result_bank_info = BankInfo::Dynamic(new_bank_name.clone());
                insts.push(AsmInst::Comment(format!("Result bank tracked as Dynamic({new_bank_name})")));
                debug!("  Updated stack-based pointer bank to trackable named value");
            }
            BankInfo::Heap(heap_bank) => {
                // Heap bank: new_bank = heap_bank + bank_delta
                let new_bank_name = naming.gep_new_bank(result_temp);
                
                // Clear any existing binding for this bank name
                mgr.clear_value_binding(&new_bank_name);
                insts.extend(mgr.take_instructions());
                
                let new_bank_reg = mgr.get_register(new_bank_name.clone());
                insts.extend(mgr.take_instructions());
                
                // Load the heap bank number first
                let heap_bank_name = naming.temp_with_context("gep", "heap_bank_base");
                let heap_bank_reg = mgr.get_register(heap_bank_name);
                insts.extend(mgr.take_instructions());
                insts.push(AsmInst::Li(heap_bank_reg, heap_bank as i16));
                
                insts.push(AsmInst::Comment(format!("Computing new bank {new_bank_name} = heap@{} + bank_delta", heap_bank)));
                insts.push(AsmInst::Add(new_bank_reg, heap_bank_reg, bank_delta_reg));
                
                mgr.free_register(heap_bank_reg);
                
                // Bind the bank value to its register so it can be tracked/reloaded
                mgr.bind_value_to_register(new_bank_name.clone(), new_bank_reg);
                result_bank_info = BankInfo::Dynamic(new_bank_name.clone());
                insts.push(AsmInst::Comment(format!("Result bank tracked as Dynamic({new_bank_name})")));
                debug!("  Updated heap-based pointer bank to trackable named value");
            }
        }

        // Move the corrected address to the result register
        insts.push(AsmInst::Add(result_addr_reg, new_addr_reg, Reg::R0));

        // Clean up temporary registers used for bank overflow
        mgr.free_register(offset_reg);
        mgr.free_register(bank_delta_reg);
        mgr.free_register(bank_size_reg);
        mgr.free_register(new_addr_reg);
    }

    // Step 5: Store bank info for the result pointer
    insts.push(AsmInst::Comment(format!("GEP: Setting bank info for {result_name} to {result_bank_info:?}")));
    mgr.set_pointer_bank(result_name.clone(), result_bank_info.clone());
    debug!(
        "  Result pointer '{result_name}' has bank info: {result_bank_info:?}"
    );
    
    // CRITICAL: Bind the result register to the result temp
    // This ensures the register manager knows what's in the register after GEP
    mgr.bind_value_to_register(result_name.clone(), result_addr_reg);

    // Free the base address register only if it's different from the result
    // and it was allocated for a constant (not a temp that might be reused)
    if base_addr_reg != result_addr_reg {
        // Check if the base register was allocated for a constant
        if let Value::FatPtr(fp) = base_ptr {
            if matches!(fp.addr.as_ref(), Value::Constant(_)) {
                // This register was allocated just for a constant, safe to free
                mgr.free_register(base_addr_reg);
                trace!("  Freed base register {base_addr_reg:?} (was constant)");
            } else {
                // Base contains a temp - keep it for potential reuse
                trace!("  Keeping base register {base_addr_reg:?} (contains temp)");
            }
        } else {
            // Base is a plain temp - keep it for potential reuse
            trace!("  Keeping base register {base_addr_reg:?} (is temp)");
        }
    } else {
        trace!("  Base register reused as result");
    }

    debug!("lower_gep complete: generated {} instructions", insts.len());
    trace!(
        "  Final register state: spill_count={}",
        mgr.get_spill_count()
    );

    insts
}

#[cfg(test)]
mod tests {
    use rcc_frontend::BankTag;
    use super::*;
    use crate::naming::new_function_naming;

    const BANK_SIZE_INSTRUCTIONS: u16 = 4096;

    #[test]
    fn test_gep_constant_offset() {
        let mut mgr = RegisterPressureManager::new(0);
        mgr.init();
        let mut naming = new_function_naming();

        // Create a base pointer in temp 10
        let base_ptr = Value::Temp(10);
        mgr.set_pointer_bank("t10".to_string(), BankInfo::Stack);

        // GEP with constant offset
        let indices = vec![Value::Constant(5)];
        let element_size = 4; // 4-byte elements

        let insts = lower_gep(&mut mgr, &mut naming, &base_ptr, &indices, element_size, 20, BANK_SIZE_INSTRUCTIONS);

        // Should generate ADD immediate instruction
        assert!(insts
            .iter()
            .any(|inst| matches!(inst, AsmInst::AddI(_, _, 20))));
    }

    #[test]
    fn test_gep_dynamic_offset() {
        let mut mgr = RegisterPressureManager::new(0);
        mgr.init();
        let mut naming = new_function_naming();

        // Create a base pointer
        let base_ptr = Value::FatPtr(rcc_frontend::ir::FatPointer {
            addr: Box::new(Value::Temp(10)),
            bank: BankTag::Stack,
        });

        // GEP with dynamic offset
        let indices = vec![Value::Temp(11)];
        let element_size = 8; // 8-byte elements (power of 2)

        let _insts = lower_gep(&mut mgr, &mut naming, &base_ptr, &indices, element_size, 20, BANK_SIZE_INSTRUCTIONS);

        // Should generate shift instruction for power-of-2 size
        // Check for shift instruction (using Sll with register, not SllI)
    }

    #[test]
    fn test_gep_bank_overflow_detection() {
        let mut mgr = RegisterPressureManager::new(0);
        mgr.init();
        let mut naming = new_function_naming();

        // Create a base pointer
        let base_ptr = Value::Temp(10);
        mgr.set_pointer_bank("t10".to_string(), BankInfo::Stack);

        // GEP with large offset that crosses bank boundary
        // Use an index that will definitely cross the bank boundary
        let large_index = (BANK_SIZE_INSTRUCTIONS + 100) as i64 / 4; // Will result in offset > BANK_SIZE
        let indices = vec![Value::Constant(large_index)];
        let element_size = 4;

        let insts = lower_gep(&mut mgr, &mut naming, &base_ptr, &indices, element_size, 20, BANK_SIZE_INSTRUCTIONS);

        // Should generate warning about bank overflow
        assert!(insts.iter().any(|inst| {
            if let AsmInst::Comment(s) = inst {
                s.contains("bank overflow") || s.contains("WARNING")
            } else {
                false
            }
        }));
    }
}
