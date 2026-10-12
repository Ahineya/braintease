//! Load instruction lowering for V2 backend
//! 
//! Handles loading values from memory with proper bank management.
//! Supports both scalar loads and fat pointer loads (2-component).

use rcc_frontend::ir::{Value, IrType as Type};
use rcc_common::TempId;
use crate::regmgmt::{RegisterPressureManager, BankInfo, BankTagValue};
use crate::naming::NameGenerator;
use rcc_codegen::{AsmInst, Reg, emit_addr_constant};
use log::{debug, trace, warn};
use rcc_frontend::BankTag;
use super::helpers::{resolve_bank_tag_to_info, get_bank_register_with_runtime_check_safe};

/// Private, pinned copies of a memory address. Advancing a multiword access
/// must neither mutate the original pointer nor alter GP/SB.
pub(super) struct MemoryCursor {
    pub addr: Reg,
    pub bank: Reg,
    limit: Reg,
}

impl MemoryCursor {
    pub fn new(mgr: &mut RegisterPressureManager, naming: &mut NameGenerator,
        insts: &mut Vec<AsmInst>, addr: Reg, bank: Reg) -> Self {
        let mut allocate = |suffix| {
            let r = mgr.get_register(naming.temp_with_context("memory", suffix));
            insts.extend(mgr.take_instructions());
            mgr.pin_register(r);
            r
        };
        let copy_addr = allocate("addr");
        let copy_bank = allocate("bank");
        let limit = allocate("limit");
        insts.push(AsmInst::Add(copy_addr, addr, Reg::R0));
        insts.push(AsmInst::Add(copy_bank, bank, Reg::R0));
        insts.push(AsmInst::Li(limit, mgr.memory_bank_size() as i16));
        Self { addr: copy_addr, bank: copy_bank, limit }
    }

    pub fn advance(&self, naming: &mut NameGenerator, insts: &mut Vec<AsmInst>) {
        let done = naming.i32_label("memory_next_word");
        // A normalized offset is at most bank_size-1 (<=65534), so this
        // increment cannot lose a 16-bit carry, even with bank_size=65535.
        insts.push(AsmInst::AddI(self.addr, self.addr, 1));
        insts.push(AsmInst::Bne(self.addr, self.limit, done.clone()));
        insts.push(AsmInst::Li(self.addr, 0));
        insts.push(AsmInst::AddI(self.bank, self.bank, 1));
        insts.push(AsmInst::Label(done));
    }

    pub fn release(self, mgr: &mut RegisterPressureManager) {
        for r in [self.addr, self.bank, self.limit] {
            mgr.unpin_register(r);
            mgr.free_register(r);
        }
    }
}

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
            insts.extend(mgr.take_instructions());
            mgr.pin_register(reg);
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
                    insts.extend(mgr.take_instructions());
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

            insts.extend(mgr.take_instructions());
            mgr.pin_register(addr_reg);

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
    
    mgr.pin_register(bank_reg);
    let dest_reg = mgr.get_register(result_name.clone());
    insts.extend(mgr.take_instructions());
    mgr.pin_register(dest_reg);
    insts.push(AsmInst::Load(dest_reg, bank_reg, addr_reg));

    if result_type.is_two_word_scalar() || result_type.is_four_word_scalar() || result_type.is_pointer() {
        let cursor = MemoryCursor::new(mgr, naming, &mut insts, addr_reg, bank_reg);
        // The cursor owns independent copies; the original pointer is no
        // longer held by value while additional destination words allocate.
        mgr.unpin_register(addr_reg);
        mgr.unpin_register(bank_reg);
        if result_type.is_two_word_scalar() {
            let hi_name = naming.i32_high_name(&result_name);
            let hi_reg = mgr.get_register(hi_name.clone());
            insts.extend(mgr.take_instructions());
            cursor.advance(naming, &mut insts);
            insts.push(AsmInst::Load(hi_reg, cursor.bank, cursor.addr));
            mgr.set_i32_high(result_name.clone(), hi_name);
            if result_type.is_f32() { mgr.set_fp32(result_name.clone()); }
        } else if result_type.is_four_word_scalar() {
            let mut extra = [String::new(), String::new(), String::new()];
            for (i, name) in extra.iter_mut().enumerate() {
                *name = naming.i64_word_name(&result_name, (i + 1) as u8);
                let r = mgr.get_register(name.clone());
                insts.extend(mgr.take_instructions());
                cursor.advance(naming, &mut insts);
                insts.push(AsmInst::Load(r, cursor.bank, cursor.addr));
            }
            mgr.set_i64_words(result_name.clone(), extra);
            if result_type.is_f64() { mgr.set_fp64(result_name.clone()); }
        } else {
            let bank_value_name = naming.load_bank_value(result_temp);
            let bank_dest_reg = mgr.get_register(bank_value_name.clone());
            insts.extend(mgr.take_instructions());
            cursor.advance(naming, &mut insts);
            insts.push(AsmInst::Load(bank_dest_reg, cursor.bank, cursor.addr));
            insts.push(AsmInst::Comment(format!(
                "Bank value in {bank_dest_reg:?} - tags: {} = Global, {} = Stack, positive = dynamic",
                BankTagValue::GLOBAL, BankTagValue::STACK
            )));
            mgr.set_pointer_bank(result_name.clone(), BankInfo::Dynamic(bank_value_name));
        }
        cursor.release(mgr);
    } else {
        mgr.unpin_register(addr_reg);
        mgr.unpin_register(bank_reg);
    }
    mgr.unpin_register(dest_reg);

    debug!("lower_load complete: generated {} instructions", insts.len());
    insts
}

#[cfg(test)]
mod tests {
    use super::*;
    use rcc_frontend::ir::FatPointer;
    use rcc_frontend::BankTag;
    use crate::instr::tests::machine::Machine;

    #[test]
    fn multiword_loads_and_stores_cross_banks_without_mutating_base_registers() {
        use crate::instr::lower_store;
        for bank_size in [1u16, 3, 17, 64000, 65535] {
            for distance in 1..=bank_size.min(4) {
                let offset = bank_size - distance;
                for ty in [Type::I32, Type::I64, Type::FatPtr(Box::new(Type::I16))] {
                    let mut mgr = RegisterPressureManager::new(0);
                    mgr.init();
                    mgr.set_memory_bank_size(bank_size);
                    let mut naming = NameGenerator::new(0);
                    let location = Value::FatPtr(FatPointer { addr: Box::new(Value::Constant(offset as i64)), bank: BankTag::Heap(5) });
                    let value = if ty.is_pointer() {
                        Value::FatPtr(FatPointer { addr: Box::new(Value::Constant(0x1234)), bank: BankTag::Heap(9) })
                    } else { Value::Constant(0x1357_2468_369a_47bci64) };
                    let expected: Vec<u16> = if ty.is_pointer() { vec![0x1234, 9] }
                        else if ty.is_i64() { vec![0x47bc, 0x369a, 0x2468, 0x1357] }
                        else { vec![0x47bc, 0x369a] };

                    let mut insts = lower_store(&mut mgr, &mut naming, &value, &location, &ty);
                    insts.extend(lower_load(&mut mgr, &mut naming, &location, &ty, 200));
                    // Store the loaded temp elsewhere too, exercising named
                    // high words and a loaded pointer's bank metadata.
                    let second = Value::FatPtr(FatPointer { addr: Box::new(Value::Constant(offset as i64)), bank: BankTag::Heap(20) });
                    insts.extend(lower_store(&mut mgr, &mut naming, &Value::Temp(200), &second, &ty));
                    let mut machine = Machine::new(bank_size);
                    machine.run(&insts);
                    let Machine { regs, memory, .. } = machine;
                    for base in [5u16, 20] {
                        for (i, &word) in expected.iter().enumerate() {
                            let linear = offset as usize + i;
                            let key = (base + (linear / bank_size as usize) as u16, (linear % bank_size as usize) as u16);
                            assert_eq!(memory.get(&key), Some(&word), "{ty:?}, bank_size={bank_size}, offset={offset}, key={key:?}");
                        }
                    }
                    assert_eq!(regs[Reg::Gp as usize], 1);
                    assert_eq!(regs[Reg::Sb as usize], 2);
                }
            }
        }
    }
    
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
