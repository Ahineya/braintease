//! Tests for Load instruction lowering

use rcc_ir::ir::{Value, IrType as Type, FatPointer, };
use crate::regmgmt::{RegisterPressureManager, BankInfo};
use crate::naming::new_function_naming;
use crate::instr::lower_load;
use rcc_codegen::{AsmInst, Reg};
use rcc_ir::BankTag;

#[test]
fn test_load_scalar_from_stack() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Create a stack pointer
    let ptr = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Temp(10)),
        bank: BankTag::Stack,
    });
    
    // Load i16 value
    let insts = lower_load(&mut mgr, &mut naming, &ptr, &Type::I16, 20);
    
    // Should have at least one LOAD instruction
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Load(_, _, _))));
    
    // Should use R13 for stack bank
    let load_found = insts.iter().any(|i| {
        if let AsmInst::Load(_, bank, _) = i {
            *bank == Reg::Sb
        } else {
            false
        }
    });
    assert!(load_found, "Should use R13 for stack bank");
}

#[test]
fn test_load_scalar_from_global() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Create a global pointer
    let ptr = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Constant(100)),
        bank: BankTag::Global,
    });
    
    // Load i16 value
    let insts = lower_load(&mut mgr, &mut naming, &ptr, &Type::I16, 30);
    
    // Should load constant address first
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Li(_, 100))));
    
    // Should use GP for global bank
    let load_found = insts.iter().any(|i| {
        if let AsmInst::Load(_, bank, _) = i {
            *bank == Reg::Gp
        } else {
            false
        }
    });
    assert!(load_found, "Should use GP for global bank");
}

#[test]
fn test_load_fat_pointer() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Create a pointer to load from
    let ptr = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Temp(15)),
        bank: BankTag::Stack,
    });
    
    // Load pointer type (fat pointer)
    let ptr_type = Type::FatPtr(Box::new(Type::I16));
    let insts = lower_load(&mut mgr, &mut naming, &ptr, &ptr_type, 40);
    
    // Should have two LOAD instructions (address and bank)
    let load_count = insts.iter().filter(|i| matches!(i, AsmInst::Load(_, _, _))).count();
    assert_eq!(load_count, 2, "Should have 2 loads for fat pointer");
    
    // Should have ADDI to calculate bank address
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(_, _, 1))));
}

#[test]
fn test_load_from_temp_pointer() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Set up a temp that is a pointer
    let ptr_temp = 25;
    mgr.set_pointer_bank(naming.temp_name(ptr_temp), BankInfo::Stack);
    
    // Load from the temp pointer
    let ptr = Value::Temp(ptr_temp);
    let insts = lower_load(&mut mgr, &mut naming, &ptr, &Type::I16, 50);
    
    // Should have LOAD instruction
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Load(_, _, _))));
    
    // Should use R13 for stack bank (from the pointer bank info)
    let load_found = insts.iter().any(|i| {
        if let AsmInst::Load(_, bank, _) = i {
            *bank == Reg::Sb
        } else {
            false
        }
    });
    assert!(load_found, "Should use R13 for stack pointer");
}

#[test]
fn test_load_from_global_variable() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Load from a global variable - simulate how lower.rs would resolve it
    let ptr = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Constant(2000)), // Simulated global address
        bank: BankTag::Global,
    });
    let insts = lower_load(&mut mgr, &mut naming, &ptr, &Type::I32, 60);
    
    // Should load the constant address
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Li(_, 2000))));
    // Should use GP for global bank
    assert!(insts.iter().any(|i| {
        if let AsmInst::Load(_, bank, _) = i {
            *bank == Reg::Gp
        } else {
            false
        }
    }));
    
    // Should have LOAD instruction with GP (global bank)
    let load_found = insts.iter().any(|i| {
        if let AsmInst::Load(_, bank, _) = i {
            *bank == Reg::Gp
        } else {
            false
        }
    });
    
    assert!(load_found, "Should use GP for global bank");
}