//! Integration tests for instruction combinations
//! 
//! Tests that verify multiple instructions work correctly together,
//! especially focusing on naming conflicts and consistency.

use rcc_frontend::ir::{Value, IrType as Type, FatPointer};
use crate::regmgmt::RegisterPressureManager;
use crate::naming::new_function_naming;
use crate::instr::{lower_load, lower_store};
use rcc_codegen::{AsmInst, Reg};
use rcc_frontend::BankTag;

#[test]
fn test_multiple_loads_no_conflicts() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Load multiple times to the same temp ID
    let ptr1 = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Constant(100)),
        bank: BankTag::Global,
    });
    
    let ptr2 = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Constant(200)),
        bank: BankTag::Stack,
    });
    
    // Both loads write to t20, but at different times
    let insts1 = lower_load(&mut mgr, &mut naming, &ptr1, &Type::I16, 20);
    let insts2 = lower_load(&mut mgr, &mut naming, &ptr2, &Type::I16, 20);
    
    // Both should generate valid instructions
    assert!(!insts1.is_empty());
    assert!(!insts2.is_empty());
    
    // First should use GP, second should use SB
    assert!(insts1.iter().any(|i| {
        if let AsmInst::Load(_, bank, _) = i {
            *bank == Reg::Gp
        } else {
            false
        }
    }));
    
    assert!(insts2.iter().any(|i| {
        if let AsmInst::Load(_, bank, _) = i {
            *bank == Reg::Sb
        } else {
            false
        }
    }));
}

#[test]
fn test_load_then_store_same_value() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Load a value
    let load_ptr = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Temp(10)),
        bank: BankTag::Stack,
    });
    
    let load_insts = lower_load(&mut mgr, &mut naming, &load_ptr, &Type::I16, 50);
    
    // Now store that loaded value
    let store_value = Value::Temp(50);
    let store_ptr = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Temp(60)),
        bank: BankTag::Global,
    });
    
    let store_insts = lower_store(&mut mgr, &mut naming, &store_value, &store_ptr, &Type::I16);
    
    // Both operations should succeed
    assert!(!load_insts.is_empty());
    assert!(!store_insts.is_empty());
    
    // The store should find t50 in a register (from the load)
    // and use it directly without additional loads
}

#[test]
fn test_load_store_fat_pointer_consistency() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Load a fat pointer
    let load_ptr = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Temp(15)),
        bank: BankTag::Stack,
    });
    
    let ptr_type = Type::FatPtr(Box::new(Type::I16));
    let load_insts = lower_load(&mut mgr, &mut naming, &load_ptr, &ptr_type, 70);
    
    // The loaded pointer (t70) should now have bank info
    let bank_info = mgr.get_pointer_bank("t70");
    assert!(bank_info.is_some(), "Loaded pointer should have bank info");
    
    // Store using the loaded pointer
    let store_value = Value::Constant(42);
    let store_ptr = Value::Temp(70);
    
    let store_insts = lower_store(&mut mgr, &mut naming, &store_value, &store_ptr, &Type::I16);
    
    // Store should use the bank info from the load
    assert!(!load_insts.is_empty());
    assert!(!store_insts.is_empty());
    
    // Should have loaded 2 components (addr + bank)
    let load_count = load_insts.iter().filter(|i| matches!(i, AsmInst::Load(_, _, _))).count();
    assert_eq!(load_count, 2, "Should load both pointer components");
}

#[test]
fn test_multiple_stores_no_conflicts() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Store multiple constants - should get unique temp names
    let value1 = Value::Constant(100);
    let value2 = Value::Constant(200);
    
    let ptr1 = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Temp(80)),
        bank: BankTag::Stack,
    });
    
    let ptr2 = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Temp(90)),
        bank: BankTag::Global,
    });
    
    let insts1 = lower_store(&mut mgr, &mut naming, &value1, &ptr1, &Type::I16);
    let insts2 = lower_store(&mut mgr, &mut naming, &value2, &ptr2, &Type::I16);
    
    // Both should generate valid instructions
    assert!(!insts1.is_empty());
    assert!(!insts2.is_empty());
    
    // Should load both constants
    assert!(insts1.iter().any(|i| matches!(i, AsmInst::Li(_, 100))));
    assert!(insts2.iter().any(|i| matches!(i, AsmInst::Li(_, 200))));
}

#[test]
fn test_load_modify_store_pattern() {
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Common pattern: load, modify, store back
    
    // 1. Load from memory
    let load_ptr = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Constant(1000)),
        bank: BankTag::Global,
    });
    
    let load_insts = lower_load(&mut mgr, &mut naming, &load_ptr, &Type::I16, 100);
    
    // 2. In real code, we'd modify t100 here (add, multiply, etc.)
    // For this test, we'll just store it back
    
    // 3. Store back to a different location
    let store_value = Value::Temp(100);
    let store_ptr = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Constant(2000)),
        bank: BankTag::Global,
    });
    
    let store_insts = lower_store(&mut mgr, &mut naming, &store_value, &store_ptr, &Type::I16);
    
    // Verify both operations succeeded
    assert!(!load_insts.is_empty());
    assert!(!store_insts.is_empty());
    
    // Load should have loaded from address 1000
    assert!(load_insts.iter().any(|i| matches!(i, AsmInst::Li(_, 1000))));
    
    // Store should store to address 2000
    assert!(store_insts.iter().any(|i| matches!(i, AsmInst::Li(_, 2000))));
}