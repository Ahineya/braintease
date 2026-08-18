//! Common helper functions for instruction lowering
//! 
//! This module contains shared utilities used across different instruction
//! lowering implementations.

use rcc_frontend::ir::{Value, FatPointer};
use rcc_frontend::BankTag;
use crate::regmgmt::{RegisterPressureManager, BankInfo, BankTagValue};
use crate::naming::NameGenerator;
use crate::globals::GlobalManager;
use rcc_codegen::{Reg, AsmInst, emit_addr_constant};
use rcc_common::TempId;
use log::warn;

/// Get register for a value
/// 
/// This function allocates or retrieves a register for the given value.
/// It handles all value types including constants, temporaries, globals,
/// function addresses, and fat pointers.
/// 
/// # Parameters
/// - `mgr`: Register pressure manager for allocation and spilling
/// - `naming`: Name generator for unique temporary names
/// - `value`: The value to get a register for
/// 
/// # Returns
/// The register containing or that will contain the value
/// 
/// # Panics
/// Panics if the value is Undef
pub fn get_value_register(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    value: &Value,
) -> Reg {
    match value {
        Value::Temp(id) => {
            let name = naming.temp_name(*id);
            mgr.get_register(name)
        }
        Value::Constant(_val) => {
            // Use the RegisterPressureManager's get_value_register which handles constants properly
            // This will emit the LI instruction internally
            mgr.get_value_register(value)
        }
        Value::Global(name) => {
            // This should never happen - globals should be resolved in lower.rs
            panic!("Unexpected Value::Global('{name}') - should have been resolved to FatPtr in lower.rs");
        }
        Value::Function(name) => {
            // Function addresses are like globals
            let func_name = naming.func_addr(name);
            
            mgr.get_register(func_name)
        }
        Value::FatPtr(fp) => {
            // For most operations, we just need the address part
            // The bank component is handled separately when needed
            get_value_register(mgr, naming, &fp.addr)
        }
        Value::ConstantArray(_) => {
            panic!("Cannot load constant array into register - arrays should be initialized separately");
        }
        Value::Undef => {
            panic!("Cannot use undefined value in instruction");
        }
    }
}

/// Calculate register need for a single value
/// 
/// This function estimates how many registers are needed for a value
/// without actually allocating them.
/// 
/// # Parameters
/// - `value`: The value to estimate register needs for
/// 
/// # Returns
/// Number of registers needed (0, 1, or 2)
pub fn calculate_value_need(value: &Value) -> usize {
    match value {
        Value::Constant(_) => 1,    // Need to load into register
        Value::ConstantArray(_) => 0, // Arrays don't go in registers
        Value::Temp(_) => 1,         // Temps need a register
        Value::Global(_) => panic!("Value::Global should be resolved in lower.rs {value:?}"), // Should be resolved to FatPtr
        Value::FatPtr(_) => 2,       // Fat pointers need 2 registers (addr + bank)
        Value::Function(_) => 1,     // Function addresses need a register
        Value::Undef => 0,           // Undefined values don't need registers
    }
}

/// Convert BankInfo to the actual bank register
/// 
/// DEPRECATED: This function will panic on Dynamic bank info.
/// Use get_bank_register_with_mgr instead which properly handles reloading.
/// 
/// This helper converts abstract bank information to concrete register:
/// - Global -> GP register
/// - Stack -> SB register  
/// - Register(r) -> the dynamic register r
/// 
/// WARNING: For Register(r), this assumes r is still valid. If the register
/// might have been spilled, the value needs to be reloaded first!
/// For Dynamic, this will panic - use get_bank_register_with_mgr instead.
#[deprecated(note = "Use get_bank_register_with_mgr instead to handle Dynamic properly")]
pub fn get_bank_register(bank_info: &BankInfo) -> Reg {
    bank_info.to_register()
}

/// Get bank register with proper reloading support
/// 
/// DEPRECATED: This function does NOT generate runtime checking for Dynamic banks.
/// Use get_bank_register_with_runtime_check_safe instead which always handles Dynamic banks correctly.
/// 
/// This version only returns the raw register for Dynamic banks without checking if it's a tag.
#[deprecated(note = "Use get_bank_register_with_runtime_check_safe instead for Dynamic banks")]
fn get_bank_register_with_mgr(
    bank_info: &BankInfo,
    mgr: &mut RegisterPressureManager
) -> Reg {
    match bank_info {
        BankInfo::Global => Reg::Gp,
        BankInfo::Stack => Reg::Sb,
        BankInfo::Register(reg) => *reg,
        BankInfo::Heap(bank) => {
            panic!("BankInfo::Heap({}) requires explicit register allocation - cannot use get_bank_register_with_mgr", bank)
        }
        BankInfo::Dynamic(name) => {
            // UNSAFE: Just returns the register without checking if it contains a tag
            // This will fail if the register contains -1 (Global) or -2 (Stack) tags
            let bank_reg = mgr.get_register(name.clone());
            bank_reg
        }
    }
}

/// Get bank register with runtime tag checking
/// 
/// This function generates runtime code to check if a dynamic bank value
/// is actually a tag (GLOBAL=-1, STACK=-2) and returns the appropriate
/// register. This is needed when loading pointers from memory where we
/// store tags instead of actual bank addresses.
/// 
/// Returns (final_bank_register, instructions_to_emit)
fn get_bank_register_with_runtime_check(
    bank_info: &BankInfo,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    context: &str
) -> (Reg, Vec<AsmInst>) {
    let mut insts = vec![];
    
    match bank_info {
        BankInfo::Global => (Reg::Gp, insts),
        BankInfo::Stack => (Reg::Sb, insts),
        BankInfo::Register(reg) => (*reg, insts),
        BankInfo::Heap(bank) => {
            // For heap banks, we need to allocate a register and load the bank number
            let result_name = naming.temp_with_context(context, "heap_bank");
            let result_reg = mgr.get_register(result_name);
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(result_reg, *bank as i16));
            (result_reg, insts)
        }
        BankInfo::Dynamic(name) => {
            // Get the register containing the bank value (might be a tag)
            let bank_val_reg = mgr.get_register(name.clone());
            insts.extend(mgr.take_instructions());
            
            // We need to check if this is a tag and convert to the actual register
            // Generate unique labels using the naming module
            let (use_global_label, use_stack_label, done_label) = naming.bank_check_labels(context);
            
            // Allocate a result register that will hold the final bank
            let result_name = naming.temp_with_context(context, "resolved_bank");
            let result_reg = mgr.get_register(result_name);
            insts.extend(mgr.take_instructions());
            
            // Check if bank_val_reg == GLOBAL tag (-1)
            let global_tag_name = naming.temp_with_context(context, "global_tag");
            let global_tag_reg = mgr.get_register(global_tag_name);
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(global_tag_reg, BankTagValue::GLOBAL));
            insts.push(AsmInst::Beq(bank_val_reg, global_tag_reg, use_global_label.clone()));
            
            // Check if bank_val_reg == STACK tag (-2)
            let stack_tag_name = naming.temp_with_context(context, "stack_tag");
            let stack_tag_reg = mgr.get_register(stack_tag_name);
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(stack_tag_reg, BankTagValue::STACK));
            insts.push(AsmInst::Beq(bank_val_reg, stack_tag_reg, use_stack_label.clone()));
            
            // Not a tag - use the value as-is (it's a dynamic bank address)
            insts.push(AsmInst::Add(result_reg, bank_val_reg, Reg::R0));
            insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done_label.clone()));
            
            // use_global_label: Use GP register
            insts.push(AsmInst::Label(use_global_label));
            insts.push(AsmInst::Add(result_reg, Reg::Gp, Reg::R0));
            insts.push(AsmInst::Beq(Reg::R0, Reg::R0, done_label.clone()));
            
            // use_stack_label: Use SB register
            insts.push(AsmInst::Label(use_stack_label));
            insts.push(AsmInst::Add(result_reg, Reg::Sb, Reg::R0));
            
            // done_label: result_reg now contains the correct bank
            insts.push(AsmInst::Label(done_label));
            
            // Free the temporary registers
            mgr.free_register(global_tag_reg);
            mgr.free_register(stack_tag_reg);
            
            (result_reg, insts)
        }
    }
}

/// Resolve a global variable name to a FatPointer with its address
/// 
/// Converts Value::Global to Value::FatPtr with the resolved address
/// from the global manager.
pub fn resolve_global_to_fatptr(
    name: &str, 
    global_manager: &GlobalManager
) -> Result<Value, String> {
    if let Some(info) = global_manager.get_global_info(name) {
        Ok(Value::FatPtr(FatPointer {
            addr: Box::new(Value::Constant(info.address as i64)),
            bank: BankTag::Global,
        }))
    } else {
        // If it's not a global variable, it might be a function
        // Functions don't have addresses in global memory, they're just labels
        // Return an error that will be caught and handled appropriately
        Err(format!("Unknown global variable: {name}"))
    }
}

/// Resolve any BankTag to concrete BankInfo
/// 
/// Converts BankTag enum values to BankInfo, handling Mixed tags specially
/// by resolving them through the register manager.
pub fn resolve_bank_tag_to_info(
    bank_tag: &BankTag,
    fp: &FatPointer,
    mgr: &RegisterPressureManager,
    naming: &NameGenerator
) -> BankInfo {
    match bank_tag {
        BankTag::Global => BankInfo::Global,
        BankTag::Stack => BankInfo::Stack,
        BankTag::Mixed => resolve_mixed_bank(fp, mgr, naming),
        BankTag::Null => panic!("NULL pointer dereference: attempted to access NULL pointer"),
        BankTag::Heap(bank) => BankInfo::Heap(*bank),
        other => panic!("Helpers: Unsupported bank type for fat pointer: {other:?}"),
    }
}

/// Resolve a Mixed bank tag to concrete BankInfo
/// 
/// For Mixed bank tags, the actual bank is determined at runtime and
/// should be tracked in the register manager for the underlying temp.
pub fn resolve_mixed_bank(
    fp: &FatPointer,
    mgr: &RegisterPressureManager,
    naming: &NameGenerator
) -> BankInfo {
    match fp.addr.as_ref() {
        Value::Temp(t) => {
            let temp_name = naming.temp_name(*t);
            mgr.get_pointer_bank(&temp_name)
                .unwrap_or_else(|| {
                    panic!("HELPERS: COMPILER BUG: No bank info for Mixed pointer '{temp_name}'. All pointers must have tracked bank information!");
                })
        }
        Value::Constant(_) => {
            panic!("FatPtr with BankTag::Mixed cannot have a constant address")
        }
        other => {
            warn!("Unexpected address type for Mixed fat ptr: {other:?}");
            BankInfo::Stack
        }
    }
}

/// Load a constant into a register
/// 
/// Allocates a register and generates an LI instruction to load the constant.
/// Returns the register and the instruction to emit.
pub fn load_constant_to_register(
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    value: i64,
    result_temp: TempId
) -> (Reg, Vec<AsmInst>) {
    let temp_name = naming.const_for_temp(result_temp);
    let reg = mgr.get_register(temp_name);
    let mut insts = mgr.take_instructions();
    insts.push(AsmInst::Li(reg, value as i16));
    (reg, insts)
}

/// Materialize a bank tag to a register
/// 
/// Takes a BankTag and produces a register containing the bank value.
/// Returns (register, instructions, owned) where owned indicates if 
/// the register should be freed after use.
pub fn materialize_bank_to_register(
    bank_tag: &BankTag,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    context: &str
) -> (Reg, Vec<AsmInst>, bool) {
    let mut insts = vec![];
    
    match bank_tag {
        BankTag::Global => {
            // Copy GP into a temp register
            let name = naming.temp_with_context(context, "bank_global");
            let r = mgr.get_register(name);
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Add(r, Reg::Gp, Reg::R0));
            (r, insts, true)
        }
        BankTag::Stack => {
            // Copy SB into a temp register
            let name = naming.temp_with_context(context, "bank_stack");
            let r = mgr.get_register(name);
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Add(r, Reg::Sb, Reg::R0));
            (r, insts, true)
        }
        BankTag::Mixed => {
            panic!("Cannot materialize Mixed bank tag without additional context - use resolve_mixed_bank first")
        }
        BankTag::Null => {
            panic!("NULL pointer dereference: attempted to use NULL pointer")
        }
        BankTag::Heap(bank) => {
            // Load the heap bank number into a register
            let name = naming.temp_with_context(context, "bank_heap");
            let r = mgr.get_register(name);
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(r, *bank as i16));
            (r, insts, true)
        }
        _ => {
            panic!("HELPERS: Unexpected bank tag type: {bank_tag:?}");
        }
    }
}

/// Get bank value for passing as function argument
/// 
/// This function preserves bank tag values (-1 for Global, -2 for Stack)
/// when preparing arguments for function calls. This is critical because
/// the callee expects to receive tag values, not resolved register values.
/// 
/// Returns (register_containing_bank_value, instructions_to_emit)
pub fn get_bank_value_for_argument(
    bank_info: &BankInfo,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    context: &str
) -> (Reg, Vec<AsmInst>) {
    let mut insts = vec![];
    
    match bank_info {
        BankInfo::Global => {
            // Pass the GLOBAL tag value (-1), not GP register
            let name = naming.temp_with_context(context, "global_tag");
            let r = mgr.get_register(name);
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(r, BankTagValue::GLOBAL));
            (r, insts)
        }
        BankInfo::Stack => {
            // Pass the STACK tag value (-2), not SB register
            let name = naming.temp_with_context(context, "stack_tag");
            let r = mgr.get_register(name);
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(r, BankTagValue::STACK));
            (r, insts)
        }
        BankInfo::Heap(bank) => {
            // Pass the heap bank number directly
            let name = naming.temp_with_context(context, "heap_bank");
            let r = mgr.get_register(name);
            insts.extend(mgr.take_instructions());
            insts.push(AsmInst::Li(r, *bank as i16));
            (r, insts)
        }
        BankInfo::Register(reg) => {
            // Already in a register, use as-is
            (*reg, insts)
        }
        BankInfo::Dynamic(name) => {
            // Dynamic values are already in the correct form (either tags or addresses)
            // Just get the register containing the value
            let bank_reg = mgr.get_register(name.clone());
            insts.extend(mgr.take_instructions());
            (bank_reg, insts)
        }
    }
}

/// Get bank register with automatic runtime checking for Dynamic banks
/// 
/// This is the SAFE version that automatically generates runtime checking
/// for Dynamic banks. Use this instead of get_bank_register_with_mgr.
/// 
/// Returns (bank_register, instructions_to_emit)
pub fn get_bank_register_with_runtime_check_safe(
    bank_info: &BankInfo,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    context: &str
) -> (Reg, Vec<AsmInst>) {
    match bank_info {
        BankInfo::Dynamic(_) | BankInfo::Heap(_) => {
            // Dynamic banks need runtime tag checking; heap banks need LI of the bank number
            get_bank_register_with_runtime_check(bank_info, mgr, naming, context)
        }
        _ => {
            // Static banks can use the simple version
            #[allow(deprecated)]
            let reg = get_bank_register_with_mgr(bank_info, mgr);
            (reg, vec![])
        }
    }
}

/// Get the address register from a pointer value
/// 
/// Handles Value::Temp, Value::FatPtr, and constants within FatPtr.
/// Returns (address_register, pointer_name_for_bank_lookup, instructions).
pub fn get_pointer_address_and_name(
    ptr_value: &Value,
    mgr: &mut RegisterPressureManager,
    naming: &mut NameGenerator,
    result_temp: TempId
) -> (Reg, String, Vec<AsmInst>) {
    let mut insts = vec![];
    
    match ptr_value {
        Value::Temp(t) => {
            let name = naming.temp_name(*t);
            let reg = mgr.get_register(name.clone());
            insts.extend(mgr.take_instructions());
            (reg, name, insts)
        }
        Value::FatPtr(fp) => {
            let addr_reg = match fp.addr.as_ref() {
                Value::Temp(t) => {
                    let name = naming.temp_name(*t);
                    let reg = mgr.get_register(name);
                    insts.extend(mgr.take_instructions());
                    reg
                }
                Value::Constant(c) => {
                    let temp_name = naming.const_for_temp(result_temp);
                    let reg = mgr.get_register(temp_name);
                    insts.extend(mgr.take_instructions());
                    insts.extend(emit_addr_constant(
                        reg,
                        *c as i16,
                        matches!(fp.bank, BankTag::Global),
                        mgr.gp_base_label(),
                    ));
                    reg
                }
                Value::Global(_) => {
                    panic!("Value::Global in FatPtr address should have been resolved in lower.rs")
                }
                _ => panic!("Invalid fat pointer address type: {:?}", fp.addr)
            };
            
            // Generate a unique key for this pointer's bank info
            let ptr_name = naming.pointer_bank_key(&format!("ptr_{result_temp}"));
            (addr_reg, ptr_name, insts)
        }
        Value::Global(_) => {
            panic!("Value::Global should have been resolved to FatPtr in lower.rs")
        }
        _ => panic!("Invalid pointer value type: {ptr_value:?}")
    }
}

/// Resolve any Value::Global references within a Value to FatPtr
/// 
/// This is used to canonicalize values before processing in instruction lowering.
pub fn canonicalize_value(
    value: &Value,
    global_manager: &GlobalManager
) -> Result<Value, String> {
    match value {
        Value::Global(name) => resolve_global_to_fatptr(name, global_manager),
        Value::FatPtr(fp) => {
            // Check if the address contains a Global that needs resolution
            match fp.addr.as_ref() {
                Value::Global(name) => {
                    if let Some(info) = global_manager.get_global_info(name) {
                        Ok(Value::FatPtr(FatPointer {
                            addr: Box::new(Value::Constant(info.address as i64)),
                            bank: fp.bank,
                        }))
                    } else {
                        Err(format!("Unknown global variable in FatPtr: {name}"))
                    }
                }
                _ => Ok(value.clone())
            }
        }
        Value::Function(name) => {
            // Function pointers are not yet implemented
            Err(format!("Function pointers not yet implemented. Cannot use function '{name}' as a value"))
        }
        _ => Ok(value.clone())
    }
}