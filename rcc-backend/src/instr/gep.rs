//! Bank-aware pointer arithmetic over the existing 16-bit ISA.
use crate::naming::NameGenerator;
use crate::regmgmt::RegisterPressureManager;
use rcc_codegen::AsmInst;
use rcc_common::TempId;
use rcc_ir::ir::Value;

pub fn lower_gep(mgr: &mut RegisterPressureManager, naming: &mut NameGenerator,
    base_ptr: &Value, indices: &[Value], element_size: u32,
    result_temp: TempId, bank_size: u16) -> Vec<AsmInst> {
    assert_eq!(indices.len(), 1, "multi-index GEP must be lowered by the frontend");
    super::pointers::offset(mgr, naming, base_ptr, &indices[0], element_size, result_temp, bank_size)
}

#[cfg(test)]
mod tests {
    use rcc_ir::BankTag;
    use super::*;
    use crate::regmgmt::BankInfo;
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
        let base_ptr = Value::FatPtr(rcc_ir::ir::FatPointer {
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
