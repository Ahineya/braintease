//! Tests for Store instruction lowering

use rcc_frontend::ir::{Value, FatPointer, };
use crate::regmgmt::{RegisterPressureManager, BankInfo};
use crate::naming::new_function_naming;
use crate::instr::lower_store;
use rcc_codegen::{AsmInst, Reg};
use rcc_frontend::BankTag;

#[test]
fn test_store_scalar_to_stack() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Store a constant to stack
    let value = Value::Constant(42);
    let ptr = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Temp(10)),
        bank: BankTag::Stack,
    });
    
    let insts = lower_store(&mut mgr, &mut naming, &value, &ptr);
    
    // Should load constant into register
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Li(_, 42))));
    
    // Should have STORE instruction
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Store(_, _, _))));
    
    // Should use R13 for stack bank
    let store_found = insts.iter().any(|i| {
        if let AsmInst::Store(_, bank, _) = i {
            *bank == Reg::Sb
        } else {
            false
        }
    });
    assert!(store_found, "Should use R13 for stack bank");
}

#[test]
fn test_store_scalar_to_global() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Store a temp to global
    let value = Value::Temp(20);
    let ptr = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Constant(200)),
        bank: BankTag::Global,
    });
    
    let insts = lower_store(&mut mgr, &mut naming, &value, &ptr);
    
    // Should load destination address
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Li(_, 200))));
    
    // Should use GP for global bank
    let store_found = insts.iter().any(|i| {
        if let AsmInst::Store(_, bank, _) = i {
            *bank == Reg::Gp
        } else {
            false
        }
    });
    assert!(store_found, "Should use GP for global bank");
}

#[test]
fn test_store_fat_pointer() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Store a fat pointer value
    let value = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Constant(100)),
        bank: BankTag::Global,
    });
    
    let ptr = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Temp(30)),
        bank: BankTag::Stack,
    });
    
    let insts = lower_store(&mut mgr, &mut naming, &value, &ptr);
    
    // Should load pointer address
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Li(_, 100))));
    // Store the Global bank tag (-1), not a copy of GP
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Li(_, -1))),
            "Should materialize global bank tag -1");
    
    // Should have two STORE instructions (address and bank)
    let store_count = insts.iter().filter(|i| matches!(i, AsmInst::Store(_, _, _))).count();
    assert_eq!(store_count, 2, "Should have 2 stores for fat pointer");
    
    // Should have ADDI to calculate bank address
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(_, _, 1))));
}

#[test]
fn test_store_to_temp_pointer() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Set up a temp that is a pointer
    let ptr_temp = 35;
    mgr.set_pointer_bank(naming.temp_name(ptr_temp), BankInfo::Global);
    
    // Store to the temp pointer
    let value = Value::Constant(99);
    let ptr = Value::Temp(ptr_temp);
    
    let insts = lower_store(&mut mgr, &mut naming, &value, &ptr);
    
    // Should load constant
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Li(_, 99))));
    
    // Should use GP for global bank (from the pointer bank info)
    let store_found = insts.iter().any(|i| {
        if let AsmInst::Store(_, bank, _) = i {
            *bank == Reg::Gp
        } else {
            false
        }
    });
    assert!(store_found, "Should use GP for global pointer");
}

#[test]
fn test_store_pointer_as_value() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Store a temp that happens to be a pointer (should store both components)
    let value_temp = 45;
    mgr.set_pointer_bank(naming.temp_name(value_temp), BankInfo::Register(Reg::A2));
    
    let value = Value::Temp(value_temp);
    let ptr = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Temp(50)),
        bank: BankTag::Stack,
    });
    
    let insts = lower_store(&mut mgr, &mut naming, &value, &ptr);
    
    // Should store both address and bank components
    let store_count = insts.iter().filter(|i| matches!(i, AsmInst::Store(_, _, _))).count();
    assert_eq!(store_count, 2, "Should have 2 stores for pointer value");
    
    // Should have ADDI to calculate bank address
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(_, _, 1))));
}

#[test]
fn test_store_to_global_variable() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Store to a global variable - simulate how lower.rs would resolve it
    let value = Value::Constant(777);
    let ptr = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Constant(3000)), // Simulated global address
        bank: BankTag::Global,
    });
    
    let insts = lower_store(&mut mgr, &mut naming, &value, &ptr);
    
    // Should load the address and value
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Li(_, 3000)))); // Address
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Li(_, 777)))); // Value to store
    
    // Should have STORE instruction with GP (global bank)
    let store_found = insts.iter().any(|i| {
        if let AsmInst::Store(_, bank, _) = i {
            *bank == Reg::Gp
        } else {
            false
        }
    });
    assert!(store_found, "Should use GP for global bank");
}