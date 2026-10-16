//! Store instruction lowering for V2 backend
//! 
//! Handles storing values to memory with proper bank management.
//! Supports both scalar stores and fat pointer stores (2-component).

use rcc_ir::ir::{Value, IrType};
use crate::regmgmt::{RegisterPressureManager, BankInfo, BankTagValue};
use crate::naming::NameGenerator;
use rcc_codegen::{AsmInst, Reg, emit_addr_constant};
use log::{debug, trace, warn};
use rcc_ir::BankTag;
use super::load::MemoryCursor;
use super::helpers::{resolve_mixed_bank, resolve_bank_tag_to_info, get_bank_register_with_runtime_check_safe, get_bank_value_for_argument, materialize_bank_to_register};

/// Lower a Store instruction to assembly
/// 
/// # Parameters
/// - `mgr`: Register pressure manager for allocation and spilling
/// - `naming`: Name generator for unique temporary names
/// - `value`: Value to store
/// - `ptr_value`: Pointer value to store to (must contain bank info)
/// 
/// # Returns
/// Vector of assembly instructions for the store operation
pub fn lower_store(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    value: &Value,
    ptr_value: &Value,
    value_type: &IrType,
) -> Vec<AsmInst> {
    debug!("lower_store: value={value:?}, ptr={ptr_value:?}");
    
    let mut insts = vec![];
    
    // Step 1: Get the value to store into a register
    // The third tuple item is Option<(Reg, bool)>: the bank register and whether we own it (must free)
    let (src_reg, is_pointer, ptr_bank_reg) = match value {
        Value::Temp(t) => {
            let name = naming.temp_name(*t);
            trace!("  Storing temp value: {name}");
            let reg = mgr.get_register(name.clone());
            insts.extend(mgr.take_instructions());
            mgr.pin_register(reg);

            let bank_src: Option<(Reg, bool)> = mgr.get_pointer_bank(&name)
                .map(|info| {
                    // Preserve v1 tags for fixed code; segmented code freezes SB
                    // when a stack pointer escapes into memory.
                    // Dynamic banks are reloaded if the original register was spilled.
                    let context = naming.store_bank_check_context();
                    let (resolved_reg, check_insts) = get_bank_value_for_argument(
                        &info,
                        mgr,
                        naming,
                        &context
                    );
                    let generated_code = !check_insts.is_empty();
                    insts.extend(check_insts);
                    insts.extend(mgr.take_instructions());
                    (resolved_reg, generated_code)
                });

            (reg, bank_src.is_some(), bank_src)

            // (reg, bank_reg.is_some(), bank_reg.map(|r| (r, false)))
        }
        Value::Constant(c) => {
            // Load constant into a register
            trace!("  Storing constant value: {c}");
            let temp_reg_name = naming.store_const_value();
            let temp_reg = mgr.get_register(temp_reg_name);
            insts.extend(mgr.take_instructions());
            if value_type.is_four_word_scalar() {
                let parts = crate::instr::i64::split_const64(*c);
                insts.push(AsmInst::Li(temp_reg, parts[0]));
            } else if value_type.is_two_word_scalar() {
                let (lo, _hi) = crate::instr::wide::split_const(*c);
                insts.push(AsmInst::Li(temp_reg, lo));
            } else {
                insts.push(AsmInst::Li(temp_reg, *c as i16));
            }
            mgr.pin_register(temp_reg);
            (temp_reg, false, None)
        }
        Value::Global(name) => {
            // This should never be reached - globals should be resolved in lower.rs
            panic!("Unexpected Value::Global('{name}') as store value - should have been resolved in lower.rs");
        }
        Value::FatPtr(fp) => {
            // Store fat pointer - need to handle both components
            trace!("  Storing fat pointer with bank {:?}", fp.bank);

            // Get address component
            let addr_reg = match fp.addr.as_ref() {
                Value::Temp(t) => {
                    let name = naming.temp_name(*t);
                    mgr.get_register(name)
                }
                Value::Constant(c) => {
                    let temp_reg_name = naming.store_fatptr_addr();
                    let temp_reg = mgr.get_register(temp_reg_name);
                    insts.extend(mgr.take_instructions());
                    insts.extend(emit_addr_constant(
                        temp_reg,
                        *c as i16,
                        matches!(fp.bank, BankTag::Global),
                        mgr.gp_base_label(),
                    ));
                    temp_reg
                }
                Value::Global(name) => {
                    // This should never happen - globals should be resolved to constants in lower.rs
                    panic!("Unexpected Value::Global('{name}') in FatPtr address - should have been resolved to Constant in lower.rs");
                }
                _ => panic!("Invalid fat pointer address type: {:?} in STORE", fp.addr),
            };

            insts.extend(mgr.take_instructions());
            mgr.pin_register(addr_reg);

            // Decide how to materialize the bank component for the value we are storing
            // Return (bank_reg, owned) — owned = true if we allocated a temp that must be freed here
            let (bank_reg, bank_owned) = match fp.bank {
                BankTag::Global => {
                    // Store the Global bank tag value
                    // When loaded back, this will be recognized as "use GP register"
                    let name = naming.store_fatptr_bank();
                    let r = mgr.get_register(name);
                    insts.extend(mgr.take_instructions());
                    insts.push(AsmInst::Li(r, BankTagValue::GLOBAL));
                    (r, true)
                }
                BankTag::Stack => {
                    // A segmented stack pointer keeps its actual bank in memory;
                    // fixed-stack code retains the original relative tag.
                    let name = naming.store_fatptr_bank();
                    let r = mgr.get_register(name);
                    insts.extend(mgr.take_instructions());
                    insts.push(if mgr.segmented_stack() { AsmInst::Move(r, Reg::Sb) }
                        else { AsmInst::Li(r, BankTagValue::STACK) });
                    (r, true)
                }
                BankTag::Mixed => {
                    // Mixed means the bank is determined at runtime
                    let bank_info = resolve_mixed_bank(fp, mgr, naming);
                    match bank_info {
                        BankInfo::Register(r) => {
                            // Use the dynamic bank register directly; we don't own it
                            (r, false)
                        }
                        BankInfo::Dynamic(name) => {
                            // Get the register for this named value (may reload if spilled)
                            let r = mgr.get_register(name);
                            insts.extend(mgr.take_instructions());
                            (r, false)
                        }
                        BankInfo::Global | BankInfo::Stack => {
                            // Need to materialize the bank register value
                            let bank_tag = if matches!(bank_info, BankInfo::Global) {
                                BankTag::Global
                            } else {
                                BankTag::Stack
                            };
                            let (r, mut materialized_insts, owned) = 
                                materialize_bank_to_register(&bank_tag, mgr, naming, "store_fatptr");
                            insts.append(&mut materialized_insts);
                            (r, owned)
                        }
                        BankInfo::Heap(bank) => {
                            // Store the heap bank number directly
                            let name = naming.store_fatptr_bank();
                            let r = mgr.get_register(name);
                            insts.extend(mgr.take_instructions());
                            insts.push(AsmInst::Li(r, bank as i16));
                            (r, true)
                        }
                    }
                }
                BankTag::Null => {
                    // Storing a NULL pointer value is OK - we use a special bank value for NULL
                    let name = naming.store_fatptr_bank();
                    let r = mgr.get_register(name);
                    insts.extend(mgr.take_instructions());
                    // Use the NULL bank tag
                    insts.push(AsmInst::Li(r, BankTagValue::NULL));
                    (r, true)
                }
                BankTag::Heap(bank) => {
                    // Store the heap bank number directly
                    let name = naming.store_fatptr_bank();
                    let r = mgr.get_register(name);
                    insts.extend(mgr.take_instructions());
                    insts.push(AsmInst::Li(r, bank as i16));
                    (r, true)
                }
                other => panic!("Store: Unsupported bank type for fat pointer: {other:?}"),
            };
            (addr_reg, true, Some((bank_reg, bank_owned)))
        }
        _ => {
            warn!("  Invalid value for store: {value:?}");
            panic!("Invalid value for store: {value:?}");
        }
    };
    
    insts.extend(mgr.take_instructions());

    // I32/F32 and I64/F64 occupy the same register slots as fat-pointer
    // (addr, bank) pairs, but they are scalars. A leftover pointer-bank
    // binding must not shrink them to a 2-word store.
    let is_pointer = is_pointer
        && !value_type.is_four_word_scalar()
        && !value_type.is_two_word_scalar();

    // Keep both source components live while resolving the destination.
    if let Some((bank_reg, _)) = ptr_bank_reg {
        mgr.pin_register(bank_reg);
    }
    
    // Step 2: Get the destination pointer address and bank
    let (dest_addr_reg, dest_ptr_name) = match ptr_value {
        Value::Temp(t) => {
            let name = naming.temp_name(*t);
            trace!("  Storing to temp pointer: {name}");
            let reg = mgr.get_register(name.clone());
            insts.extend(mgr.take_instructions());
            mgr.pin_register(reg);
            (reg, name)
        }
        Value::FatPtr(fp) => {
            // Fat pointer has explicit address and bank
            trace!("  Storing to fat pointer with bank {:?}", fp.bank);
            let addr_reg = match fp.addr.as_ref() {
                Value::Temp(t) => {
                    let name = naming.temp_name(*t);
                    mgr.get_register(name)
                }
                Value::Constant(c) => {
                    let temp_reg_name = naming.store_dest_addr();
                    let temp_reg = mgr.get_register(temp_reg_name);
                    insts.extend(mgr.take_instructions());
                    insts.extend(emit_addr_constant(
                        temp_reg,
                        *c as i16,
                        matches!(fp.bank, BankTag::Global),
                        mgr.gp_base_label(),
                    ));
                    trace!("  Loaded destination address {c} into {temp_reg:?}");
                    temp_reg
                }
                Value::Global(name) => {
                    // This should never happen - globals should be resolved to constants in lower.rs
                    panic!("Unexpected Value::Global('{name}') in FatPtr destination address - should have been resolved to Constant in lower.rs");
                }
                _ => panic!("Invalid fat pointer address type: {:?} in STORE", fp.addr),
            };
            
            insts.extend(mgr.take_instructions());
            mgr.pin_register(addr_reg);

            // Set bank info for the pointer
            let bank_info = resolve_bank_tag_to_info(&fp.bank, fp, mgr, naming);

            let dest_ptr_key = naming.pointer_bank_key("dest_ptr");
            mgr.set_pointer_bank(dest_ptr_key.clone(), bank_info);
            
            (addr_reg, dest_ptr_key)
        }
        Value::Global(name) => {
            // This should never happen - globals should be resolved to FatPtr in lower.rs
            panic!("Unexpected Value::Global('{name}') as store destination - should have been resolved to FatPtr in lower.rs");
        }
        _ => {
            warn!("  Invalid pointer value for store: {ptr_value:?}");
            panic!("Invalid pointer value for store")
        }
    };
    
    insts.extend(mgr.take_instructions());
    
    // Step 3: Get the bank register for the destination
    let dest_bank_info = mgr.get_pointer_bank(&dest_ptr_name)
        .unwrap_or_else(|| {
            panic!("STORE: COMPILER BUG: No bank info for pointer '{dest_ptr_name}'. All pointers must have tracked bank information!");
        });
    
    debug!("  Destination pointer {dest_ptr_name} has bank info: {dest_bank_info:?}");
    
    // Use safe runtime checking for all bank types
    let context = naming.store_bank_check_context();
    let (dest_bank_reg, check_insts) = get_bank_register_with_runtime_check_safe(
        &dest_bank_info, 
        mgr, 
        naming, 
        &context
    );
    insts.extend(check_insts);
    insts.extend(mgr.take_instructions());

    mgr.pin_register(dest_bank_reg);
    
    // Step 4: Generate STORE instruction(s)
    
    // Store the main value
    let store_inst = AsmInst::Store(src_reg, dest_bank_reg, dest_addr_reg);
    trace!("  Generated STORE: {store_inst:?}");
    insts.push(store_inst);

    let storing_i64 = value_type.is_four_word_scalar()
        || matches!(value, Value::Temp(t) if mgr.get_i64_words(&naming.temp_name(*t)).is_some());
    let storing_i32 = value_type.is_two_word_scalar()
        || matches!(value, Value::Temp(t) if mgr.get_i32_high(&naming.temp_name(*t)).is_some());
    if is_pointer || storing_i32 || storing_i64 {
        let cursor = MemoryCursor::new(mgr, naming, &mut insts, dest_addr_reg, dest_bank_reg);
        let words = if storing_i64 && !is_pointer { 4 } else { 2 };
        for i in 1..words {
            let (word, temporary) = if is_pointer {
                (ptr_bank_reg.expect("pointer value has a bank").0, false)
            } else {
                match value {
                    Value::Temp(t) => {
                        let name = naming.temp_name(*t);
                        let word_name = if storing_i64 {
                            mgr.get_i64_words(&name).map(|parts| parts[i - 1].clone())
                        } else {
                            mgr.get_i32_high(&name)
                        };
                        if let Some(word_name) = word_name {
                            let r = mgr.get_register(word_name);
                            insts.extend(mgr.take_instructions());
                            (r, false)
                        } else {
                            // Narrow scalar stored into a wider slot.
                            let r = mgr.get_register(naming.temp_with_context("store", "sign"));
                            insts.extend(mgr.take_instructions());
                            insts.push(AsmInst::Slt(r, src_reg, Reg::R0));
                            insts.push(AsmInst::Sub(r, Reg::R0, r));
                            (r, true)
                        }
                    }
                    Value::Constant(c) => {
                        let parts = crate::instr::i64::split_const64(*c);
                        let r = mgr.get_register(naming.store_const_value());
                        insts.extend(mgr.take_instructions());
                        insts.push(AsmInst::Li(r, parts[i]));
                        (r, true)
                    }
                    _ => panic!("Multiword store of unsupported value {value:?}"),
                }
            };
            cursor.advance(naming, &mut insts);
            insts.push(AsmInst::Store(word, cursor.bank, cursor.addr));
            if temporary { mgr.free_register(word); }
        }
        cursor.release(mgr);
    }

    mgr.unpin_register(src_reg);
    if let Some((bank_reg, bank_owned)) = ptr_bank_reg {
        mgr.unpin_register(bank_reg);
        if bank_owned {
            mgr.free_register(bank_reg);
        }
    }
    mgr.unpin_register(dest_addr_reg);
    mgr.unpin_register(dest_bank_reg);
    
    // Free temporary registers if we allocated them for constants
    match value {
        Value::Constant(_) => mgr.free_register(src_reg),
        Value::FatPtr(fp) => {
            // Only free if the address was a constant that we loaded into a temp register
            if matches!(fp.addr.as_ref(), Value::Constant(_)) {
                mgr.free_register(src_reg); // addr_reg
            }
            // For Temp addresses, don't free - the value might be used again
        }
        _ => {
            // For temps, we don't free src_reg because it's still holding the value
            // and might be needed again. The register allocator will handle it.
        }
    }
    
    // Free the destination address register if it was a constant or FatPtr
    // For temps, we keep it because it's the actual pointer value
    match ptr_value {
        Value::FatPtr(fp) => {
            // If the address was a constant, we allocated a temp register
            if matches!(fp.addr.as_ref(), Value::Constant(_)) {
                mgr.free_register(dest_addr_reg);
            }
            // For temp addresses, don't free - it's still the pointer value
        }
        _ => {
            // For Value::Temp pointers, don't free - they're still valid pointers
        }
    }
    
    debug!("lower_store complete: generated {} instructions", insts.len());
    insts
}

#[cfg(test)]
mod tests {
    use super::*;
    use rcc_ir::ir::{FatPointer};
    use rcc_ir::BankTag;
    
    #[test]
    #[should_panic(expected = "Unexpected Value::Global('test_global')")]
    fn test_panic_on_unresolved_global_as_store_value() {
        let mut mgr = RegisterPressureManager::new(10);
        let mut naming = NameGenerator::new(0);
        
        // Try to store a Global value directly - should panic
        let value = Value::Global("test_global".to_string());
        let ptr = Value::FatPtr(FatPointer {
            addr: Box::new(Value::Constant(0)),
            bank: BankTag::Global,
        });
        
        // This should panic
        lower_store(&mut mgr, &mut naming, &value, &ptr, &rcc_ir::ir::IrType::I16);
    }
    
    #[test]
    #[should_panic(expected = "Unexpected Value::Global('test_global') as store destination")]
    fn test_panic_on_unresolved_global_as_store_destination() {
        let mut mgr = RegisterPressureManager::new(10);
        let mut naming = NameGenerator::new(0);
        
        // Try to store to a Global directly - should panic
        let value = Value::Constant(42);
        let ptr = Value::Global("test_global".to_string());
        
        // This should panic
        lower_store(&mut mgr, &mut naming, &value, &ptr, &rcc_ir::ir::IrType::I16);
    }
    
    #[test]
    #[should_panic(expected = "Unexpected Value::Global('test_global') in FatPtr address")]
    fn test_panic_on_unresolved_global_in_fatptr_value() {
        let mut mgr = RegisterPressureManager::new(10);
        let mut naming = NameGenerator::new(0);
        
        // Try to store a FatPtr with Global in its address - should panic
        let value = Value::FatPtr(FatPointer {
            addr: Box::new(Value::Global("test_global".to_string())),
            bank: BankTag::Global,
        });
        let ptr = Value::FatPtr(FatPointer {
            addr: Box::new(Value::Constant(0)),
            bank: BankTag::Global,
        });
        
        // This should panic
        lower_store(&mut mgr, &mut naming, &value, &ptr, &rcc_ir::ir::IrType::I16);
    }
    
    #[test]
    #[should_panic(expected = "Unexpected Value::Global('test_global') in FatPtr destination")]
    fn test_panic_on_unresolved_global_in_fatptr_destination() {
        let mut mgr = RegisterPressureManager::new(10);
        let mut naming = NameGenerator::new(0);
        
        // Try to store to a FatPtr with Global in its address - should panic
        let value = Value::Constant(42);
        let ptr = Value::FatPtr(FatPointer {
            addr: Box::new(Value::Global("test_global".to_string())),
            bank: BankTag::Global,
        });
        
        // This should panic
        lower_store(&mut mgr, &mut naming, &value, &ptr, &rcc_ir::ir::IrType::I16);
    }
    
    #[test]
    #[should_panic(expected = "NULL pointer dereference: attempted to access NULL pointer")]
    fn test_store_through_null_pointer() {
        let mut mgr = RegisterPressureManager::new(10);
        let mut naming = NameGenerator::new(0);
        
        // Try to store through a NULL pointer - should panic
        let value = Value::Constant(42);
        let null_ptr = Value::FatPtr(FatPointer {
            addr: Box::new(Value::Constant(0)),
            bank: BankTag::Null,
        });
        
        // This should panic
        lower_store(&mut mgr, &mut naming, &value, &null_ptr, &IrType::I16);
    }
    
    #[test]
    #[should_panic(expected = "NULL pointer dereference: attempted to access NULL pointer")]
    fn test_store_null_pointer_value() {
        let mut mgr = RegisterPressureManager::new(10);
        let mut naming = NameGenerator::new(0);
        
        // Try to store a NULL pointer value - the value itself is OK,
        // but storing through NULL destination should panic
        let null_value = Value::FatPtr(FatPointer {
            addr: Box::new(Value::Constant(0)),
            bank: BankTag::Null,
        });
        let null_dest = Value::FatPtr(FatPointer {
            addr: Box::new(Value::Constant(0)),
            bank: BankTag::Null,
        });
        
        // This should panic because of NULL destination
        lower_store(&mut mgr, &mut naming, &null_value, &null_dest, &IrType::I16);
    }
    
    #[test]
    fn test_store_null_pointer_as_value_to_valid_location() {
        let mut mgr = RegisterPressureManager::new(10);
        let mut naming = NameGenerator::new(0);
        
        // Storing a NULL pointer value to a valid location should work
        let null_value = Value::FatPtr(FatPointer {
            addr: Box::new(Value::Constant(0)),
            bank: BankTag::Null,
        });
        let valid_dest = Value::FatPtr(FatPointer {
            addr: Box::new(Value::Constant(100)),
            bank: BankTag::Global,
        });
        
        // This should succeed - we can store NULL pointers, just not through them
        let insts = lower_store(&mut mgr, &mut naming, &null_value, &valid_dest, &IrType::I16);
        assert!(!insts.is_empty());
    }
}
