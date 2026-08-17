//! Comprehensive tests for parameter loading with different type combinations
//! 
//! These tests ensure that the calling convention correctly handles:
//! - Pure scalar parameters
//! - Pure fat pointer parameters  
//! - Mixed scalar and fat pointer parameters
//! - Edge cases like partial register usage

use crate::function::calling_convention::{CallingConvention};
use crate::regmgmt::RegisterPressureManager;
use crate::naming::new_function_naming;
use rcc_frontend::ir::IrType;
use rcc_codegen::{AsmInst, Reg};

#[test]
fn test_all_scalars_in_registers() {
    // I16/I8 take one slot; I32 takes two (like a fat pointer).
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    let mut naming = new_function_naming();

    let param_types = vec![
        (0, IrType::I16),
        (1, IrType::I8),
        (2, IrType::I32),
        (3, IrType::I16),
    ];

    let (insts, _reg, _bank_reg) = cc.load_param(0, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Add(_, Reg::A0, Reg::R0))),
            "Parameter 0 should be in A0");

    let (insts, _reg, _bank_reg) = cc.load_param(1, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Add(_, Reg::A1, Reg::R0))),
            "Parameter 1 should be in A1");

    let (insts, _reg, _bank_reg) = cc.load_param(2, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Add(_, Reg::A2, Reg::R0))),
            "Parameter 2 (I32 low word) should be in A2");

    // I32 occupied A2-A3, so parameter 3 is the first stack slot.
    let (insts, _reg, _bank_reg) = cc.load_param(3, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sc, Reg::Fp, -7))),
            "Parameter 3 should be on stack at FP-7 after I32 filled A2-A3");
}

#[test]
fn test_two_fat_pointers_fill_registers() {
    // Test that 2 fat pointers completely fill A0-A3
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    let mut naming = new_function_naming();
    
    let param_types = vec![
        (0, IrType::FatPtr(Box::new(IrType::I16))),
        (1, IrType::FatPtr(Box::new(IrType::I8))),
    ];
    
    // First fat pointer should use A0-A1
    let (insts, _reg, _bank_reg) = cc.load_param(0, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(_, Reg::A0, Reg::R0))),
            "First fat pointer address should be in A0");
    
    // Note: For fat pointers, we need to check if it's trying to load the bank part
    // But our current load_param only returns one register, so this might need adjustment
}

#[test]
fn test_scalar_fatptr_scalar_layout() {
    // Test: scalar(A0), fat_ptr(A1-A2), scalar(A3)
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    let mut naming = new_function_naming();
    
    let param_types = vec![
        (0, IrType::I16),                           // A0
        (1, IrType::FatPtr(Box::new(IrType::I16))), // A1-A2
        (2, IrType::I16),                           // A3
    ];
    
    // Check parameter 0 is in A0
    let (insts, _reg, _bank_reg) = cc.load_param(0, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(_, Reg::A0, Reg::R0))),
            "Parameter 0 should be in A0");
    
    // Check parameter 2 is in A3 (param 1 takes A1-A2)
    let (insts, _reg, _bank_reg) = cc.load_param(2, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(_, Reg::A3, Reg::R0))),
            "Parameter 2 should be in A3 after fat pointer in A1-A2");
}

#[test]
fn test_fatptr_scalar_scalar_with_overflow() {
    // Test: fat_ptr(A0-A1), scalar(A2), scalar(A3), scalar(stack)
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    let mut naming = new_function_naming();
    
    let param_types = vec![
        (0, IrType::FatPtr(Box::new(IrType::I16))), // A0-A1
        (1, IrType::I16),                           // A2
        (2, IrType::I16),                           // A3
        (3, IrType::I16),                           // Stack
    ];
    
    // Parameter 3 should be on stack at FP-7
    let (insts, _reg, _bank_reg) = cc.load_param(3, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sc, Reg::Fp, -7))),
            "Parameter 3 should be on stack at FP-7");
}

#[test]
fn test_three_scalars_then_fatptr() {
    // Test: scalar(A0), scalar(A1), scalar(A2), fat_ptr(A3 + stack)
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    let mut naming = new_function_naming();
    
    let param_types = vec![
        (0, IrType::I16),                           // A0
        (1, IrType::I16),                           // A1
        (2, IrType::I16),                           // A2
        (3, IrType::FatPtr(Box::new(IrType::I16))), // A3 for addr, stack for bank
    ];
    
    // Parameter 3 (fat pointer) should have address in A3
    let (_insts, _reg, _bank_reg) = cc.load_param(3, &param_types, &mut pm, &mut naming);
    // Since it's partially in register, it should load from A3
    // But the bank part would be on stack - our current implementation might need adjustment here
}

#[test]
fn test_all_stack_parameters() {
    // Test: 4 scalars in registers, then multiple params on stack
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    let mut naming = new_function_naming();
    
    let param_types = vec![
        (0, IrType::I16),                           // A0
        (1, IrType::I16),                           // A1
        (2, IrType::I16),                           // A2
        (3, IrType::I16),                           // A3
        (4, IrType::I16),                           // Stack -7
        (5, IrType::FatPtr(Box::new(IrType::I16))), // Stack -8, -9
        (6, IrType::I16),                           // Stack -10
    ];
    
    // Parameter 4: first stack param at FP-7
    let (insts, _reg, _bank_reg) = cc.load_param(4, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sc, Reg::Fp, -7))),
            "Parameter 4 should be at FP-7");
    
    // Parameter 6: after fat pointer at FP-10
    let (insts, _reg, _bank_reg) = cc.load_param(6, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sc, Reg::Fp, -10))),
            "Parameter 6 should be at FP-10 (after fat pointer)");
}

#[test]
fn test_complex_mixed_layout() {
    // Complex test with multiple fat pointers and scalars
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    let mut naming = new_function_naming();
    
    let param_types = vec![
        (0, IrType::FatPtr(Box::new(IrType::I16))), // A0-A1
        (1, IrType::I16),                           // A2
        (2, IrType::FatPtr(Box::new(IrType::I8))),  // A3 + stack for bank
        (3, IrType::I16),                           // Stack
        (4, IrType::FatPtr(Box::new(IrType::I32))), // Stack (2 words)
        (5, IrType::I8),                            // Stack
        (6, IrType::I16),                           // Stack
    ];
    
    // Parameter 2: fat pointer partially in register (A3) and stack
    // This is a tricky case that needs special handling
    
    // Parameter 3: first fully stack parameter
    let (insts, _reg, _bank_reg) = cc.load_param(3, &param_types, &mut pm, &mut naming);
    // Should be at FP-7 (base -6, then -1 for the bank part of param 2 that's on stack)
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sc, Reg::Fp, offset) if *offset <= -7)),
            "Parameter 3 should be on stack");
    
    // Parameter 5: scalar after fat pointer on stack
    let (insts, _reg, _bank_reg) = cc.load_param(5, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sc, Reg::Fp, offset) if *offset <= -10)),
            "Parameter 5 should be deep in stack after fat pointers");
}

#[test]
fn test_empty_parameter_list() {
    // Test function with no parameters
    let _cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    let mut _naming = new_function_naming();
    
    let _param_types: Vec<(rcc_common::TempId, IrType)> = vec![];
    
    // This should handle gracefully even though there are no params
    // (though trying to load param 0 would be invalid)
}

#[test]
fn test_single_fat_pointer() {
    // Test single fat pointer parameter
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    let mut naming = new_function_naming();
    
    let param_types = vec![
        (0, IrType::FatPtr(Box::new(IrType::I16))), // A0-A1
    ];
    
    // Address is copied from A0. Bank is copied *out of* A1 into an
    // allocatable register so later allocation cannot clobber the tag.
    let (insts, _addr_reg, bank_reg) = cc.load_param(0, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(_, Reg::A0, Reg::R0))),
            "Fat pointer address should be copied from A0");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(_, Reg::A1, Reg::R0))),
            "Fat pointer bank should be copied from A1");
    assert!(bank_reg.is_some(), "Fat pointer should have a bank register");
    assert_ne!(bank_reg, Some(Reg::A1), "Fat pointer bank must not remain in A1");
}

#[test]
fn test_maximum_register_usage() {
    // Test that exactly fills all 4 argument registers
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    let mut naming = new_function_naming();
    
    // Two fat pointers = 4 registers exactly
    let param_types = vec![
        (0, IrType::FatPtr(Box::new(IrType::I16))), // A0-A1
        (1, IrType::FatPtr(Box::new(IrType::I16))), // A2-A3
        (2, IrType::I16),                           // Stack
    ];
    
    // Parameter 2 should be first on stack
    let (insts, _reg, _bank_reg) = cc.load_param(2, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sc, Reg::Fp, -7))),
            "Parameter 2 should be first stack parameter at FP-7");
}

#[test]
fn test_stack_offset_calculation_accuracy() {
    // Detailed test of stack offset calculations
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    let mut naming = new_function_naming();
    
    let param_types = vec![
        (0, IrType::I16),                           // A0
        (1, IrType::I16),                           // A1
        (2, IrType::I16),                           // A2
        (3, IrType::I16),                           // A3
        (4, IrType::I16),                           // Stack: FP-7
        (5, IrType::I16),                           // Stack: FP-8
        (6, IrType::FatPtr(Box::new(IrType::I16))), // Stack: FP-9, FP-10
        (7, IrType::I16),                           // Stack: FP-11
        (8, IrType::FatPtr(Box::new(IrType::I16))), // Stack: FP-12, FP-13
        (9, IrType::I16),                           // Stack: FP-14
    ];
    
    // Verify each stack parameter has correct offset
    let expected_offsets = [
        (4, -7),   // First stack param
        (5, -8),   // Second stack param
        (6, -9),   // Fat pointer address (bank at -10)
        (7, -11),  // After fat pointer
        (8, -12),  // Another fat pointer address (bank at -13)
        (9, -14),  // Final scalar
    ];
    
    for (param_idx, expected_offset) in expected_offsets {
        let (insts, _reg, _bank_reg) = cc.load_param(param_idx, &param_types, &mut pm, &mut naming);
        assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sc, Reg::Fp, offset) if *offset == expected_offset)),
                "Parameter {} should be at FP{}", param_idx, expected_offset);
    }
}