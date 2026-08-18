//! Load instruction lowering for V2 backend
//! 
//! Handles loading values from memory with proper bank management.
//! Supports both scalar loads and fat pointer loads (2-component).

use rcc_frontend::ir::{Value, IrType as Type};
use rcc_common::TempId;
use crate::regmgmt::{RegisterPressureManager, BankInfo, BankTagValue};
use crate::naming::NameGenerator;
use rcc_codegen::{AsmInst, emit_addr_constant};
use log::{debug, trace, warn};
use rcc_frontend::BankTag;
use super::helpers::{resolve_bank_tag_to_info, get_bank_register_with_runtime_check_safe};

/// Lower a Load instruction to assembly
/// 
/// # Parameters
/// - `mgr`: Register pressure manager for allocation and spilling
/// - `naming`: Name generator for unique temporary names
/// - `ptr_value`: Pointer value to load from (must contain bank info)
/// - `result_type`: Type of the value being loaded
/// - `result_temp`: Temp ID for the result
/// 
/// # Returns
/// Vector of assembly instructions for the load operation
pub fn lower_load(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    ptr_value: &Value,
    result_type: &Type,
    result_temp: TempId,
) -> Vec<AsmInst> {
    debug!("lower_load: ptr={ptr_value:?}, type={result_type:?}, result=t{result_temp}");
    
    let mut insts = vec![];
    let result_name = naming.temp_name(result_temp);
    
    // Step 1: Get the pointer address register
    let (addr_reg, ptr_name) = match ptr_value {
        Value::Temp(t) => {
            let name = naming.temp_name(*t);
            trace!("  Loading from temp pointer: {name}");
            let reg = mgr.get_register(name.clone());
            (reg, name)
        }
        Value::FatPtr(fp) => {
            // Fat pointer has explicit address and bank
            trace!("  Loading from fat pointer with bank {:?}", fp.bank);
            let addr_reg = match fp.addr.as_ref() {
                Value::Temp(t) => {
                    let name = naming.temp_name(*t);
                    mgr.get_register(name)
                }
                Value::Constant(c) => {
                    // Load constant address into register
                    let temp_reg_name = naming.load_const_addr(result_temp);
                    let temp_reg = mgr.get_register(temp_reg_name);
                    insts.extend(emit_addr_constant(
                        temp_reg,
                        *c as i16,
                        matches!(fp.bank, BankTag::Global),
                        mgr.gp_base_label(),
                    ));
                    trace!("  Loaded constant address {c} into {temp_reg:?}");
                    temp_reg
                }
                _ => {
                    warn!("  Unexpected address type in fat pointer: {:?}", fp.addr);
                    panic!("Invalid fat pointer address type in LOAD: {:?}", fp.addr);
                }
            };

            // Determine how to source the bank for this fat pointer
            let bank_info = resolve_bank_tag_to_info(&fp.bank, fp, mgr, naming);
            
            // Determine the key to use for tracking this pointer's bank
            let ptr_name = if matches!(fp.bank, BankTag::Mixed) {
                // For Mixed, use the temp name if available
                match fp.addr.as_ref() {
                    Value::Temp(t) => naming.temp_name(*t),
                    _ => naming.load_src_ptr_bank(result_temp),
                }
            } else {
                // For Global/Stack, use a unique key
                naming.load_src_ptr_bank(result_temp)
            };
            
            mgr.set_pointer_bank(ptr_name.clone(), bank_info);

            (addr_reg, ptr_name)
        }
        Value::Global(name) => {
            // This should never happen - globals should be resolved to FatPtr in lower.rs
            panic!("Unexpected Value::Global('{name}') as load source - should have been resolved to FatPtr in lower.rs");
        }
        _ => {
            warn!("  Invalid pointer value for load: {ptr_value:?}");
            panic!("Invalid pointer value for load")
        }
    };
    
    // Step 2: Get the bank register based on pointer's bank info
    let bank_info = mgr.get_pointer_bank(&ptr_name)
        .unwrap_or_else(|| {
            panic!("LOAD: COMPILER BUG: No bank info for pointer '{ptr_name}'. All pointers must have tracked bank information!");
        });
    
    debug!("  Pointer {ptr_name} has bank info: {bank_info:?}");
    insts.push(AsmInst::Comment(format!("LOAD: Pointer {ptr_name} has bank info: {bank_info:?}")));
    
    // Use safe runtime checking for all bank types
    // Use proper naming mechanism to generate unique context
    let context = naming.load_bank_check_context(result_temp);
    let (bank_reg, check_insts) = get_bank_register_with_runtime_check_safe(
        &bank_info, 
        mgr, 
        naming, 
        &context
    );
    insts.extend(check_insts);
    
    insts.push(AsmInst::Comment(format!("LOAD: Using bank register {bank_reg:?} for load")));
    trace!("  Using {bank_reg:?} for bank");
    
    // Step 3: Allocate destination register and generate LOAD instruction
    // Pin address and bank so dest allocation cannot spill/reuse them.
    mgr.pin_register(addr_reg);
    mgr.pin_register(bank_reg);
    let dest_reg = mgr.get_register(result_name.clone());
    debug!("  Allocated {dest_reg:?} for result {result_name}");
    
    // Take any instructions generated by register allocation
    insts.extend(mgr.take_instructions());
    
    // Generate the actual LOAD instruction
    let load_inst = AsmInst::Load(dest_reg, bank_reg, addr_reg);
    trace!("  Generated LOAD: {load_inst:?}");
    insts.push(load_inst);

    // I32: also load the high word at addr+1
    if result_type.is_wide() || result_type.is_f32() {
        mgr.pin_register(dest_reg);
        let hi_name = naming.i32_high_name(&result_name);
        let hi_reg = mgr.get_register(hi_name.clone());
        insts.extend(mgr.take_instructions());
        insts.push(AsmInst::AddI(rcc_codegen::Reg::Sc, addr_reg, 1));
        insts.push(AsmInst::Load(hi_reg, bank_reg, rcc_codegen::Reg::Sc));
        mgr.bind_value_to_register(hi_name.clone(), hi_reg);
        mgr.set_i32_high(result_name.clone(), hi_name);
        if result_type.is_f32() {
            mgr.set_fp32(result_name.clone());
        }
        mgr.unpin_register(dest_reg);
        debug!("  two-word loaded: lo in {dest_reg:?}, hi in {hi_reg:?}");
    }

    if result_type.is_i64() || result_type.is_f64() {
        mgr.pin_register(dest_reg);
        let mut extra = [String::new(), String::new(), String::new()];
        for i in 0..3 {
            extra[i] = naming.i64_word_name(&result_name, (i + 1) as u8);
            let r = mgr.get_register(extra[i].clone());
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::AddI(rcc_codegen::Reg::Sc, addr_reg, (i + 1) as i16));
            insts.push(AsmInst::Load(r, bank_reg, rcc_codegen::Reg::Sc));
            mgr.bind_value_to_register(extra[i].clone(), r);
        }
        mgr.set_i64_words(result_name.clone(), extra);
        if result_type.is_f64() {
            mgr.set_fp64(result_name.clone());
        }
        mgr.unpin_register(dest_reg);
        debug!("  four-word loaded into {dest_reg:?} + extra words");
    }

    mgr.unpin_register(addr_reg);
    mgr.unpin_register(bank_reg);
    
    // Step 4: If loading a fat pointer, also load the bank component
    if result_type.is_pointer() {
        debug!("  Result is a pointer, loading bank component");
        
        // Calculate address for bank component (addr + 1)
        let bank_addr_name = naming.load_bank_addr(result_temp);
        let bank_addr_reg = mgr.get_register(bank_addr_name);
        insts.extend(mgr.take_instructions());
        
        insts.push(AsmInst::AddI(bank_addr_reg, addr_reg, 1));
        trace!("  Bank component at address {addr_reg:?} + 1");
        
        // Load the bank value with a trackable name
        let bank_value_name = naming.load_bank_value(result_temp);
        let bank_dest_reg = mgr.get_register(bank_value_name.clone());
        insts.extend(mgr.take_instructions());
        
        // IMPORTANT: Both components of a FatPtr are stored at the same location,
        // so they must be loaded using the same bank register!
        let bank_load = AsmInst::Load(bank_dest_reg, bank_reg, bank_addr_reg);
        trace!("  Generated bank LOAD: {bank_load:?}");
        insts.push(bank_load);
        
        // Store bank info for the loaded pointer
        // IMPORTANT: We need to check if the loaded bank value is a special tag
        // that indicates a static bank (Global/Stack) rather than a dynamic address
        
        // We need to generate code to check the loaded bank value at runtime
        // and set up the proper bank register based on the tag
        
        // For now, we'll track it as Dynamic and let the runtime handle it
        // TODO: Generate runtime code to check for bank tags and use appropriate registers
        
        if result_type.is_pointer() {
            // Add a comment about the bank tag interpretation
            insts.push(AsmInst::Comment(format!(
                "Bank value in {bank_dest_reg:?} - tags: {} = Global, {} = Stack, positive = dynamic",
                BankTagValue::GLOBAL,
                BankTagValue::STACK
            )));
            
            // Bind the bank value so it's tracked in the register manager
            mgr.bind_value_to_register(bank_value_name.clone(), bank_dest_reg);
            // Track that this pointer's bank is in a named value (not just a register)
            // This allows the bank to be reloaded if the register gets spilled
            mgr.set_pointer_bank(result_name.clone(), BankInfo::Dynamic(bank_value_name.clone()));
        }
        debug!("  Fat pointer loaded: addr in {dest_reg:?}, bank in {bank_dest_reg:?} (tracked as '{bank_value_name}')");
        
        // Free the temporary bank address register
        mgr.free_register(bank_addr_reg);
    }
    
    debug!("lower_load complete: generated {} instructions", insts.len());
    insts
}

#[cfg(test)]
mod tests {
    use super::*;
    use rcc_frontend::ir::FatPointer;
    use rcc_frontend::BankTag;
    
    #[test]
    #[should_panic(expected = "NULL pointer dereference")]
    fn test_load_from_null_pointer() {
        let mut mgr = RegisterPressureManager::new(10);
        let mut naming = NameGenerator::new(0);
        
        // Create a NULL pointer
        let null_ptr = Value::FatPtr(FatPointer {
            addr: Box::new(Value::Constant(0)),
            bank: BankTag::Null,
        });
        
        // Attempt to load from NULL pointer - should panic
        let result_type = Type::I8;
        let result_temp = 1;
        lower_load(&mut mgr, &mut naming, &null_ptr, &result_type, result_temp);
    }

    #[test]
    fn test_load_from_valid_pointer() {
        let mut mgr = RegisterPressureManager::new(10);
        let mut naming = NameGenerator::new(0);

        // Create a valid global pointer
        let valid_ptr = Value::FatPtr(FatPointer {
            addr: Box::new(Value::Constant(100)),
            bank: BankTag::Global,
        });

        // Load from valid pointer - should succeed
        let result_type = Type::I8;
        let result_temp = 1;
        let insts = lower_load(&mut mgr, &mut naming, &valid_ptr, &result_type, result_temp);
        
        // Should generate at least one LOAD instruction
        assert!(!insts.is_empty());
        assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Load(_, _, _))));
    }
}