//! End-to-end tests for function calls
//! 
//! These tests verify that the calling convention works correctly
//! from caller to callee, ensuring that parameters passed by the caller
//! are correctly received by the callee.

use crate::function::calling_convention::{CallingConvention, CallArg};
use crate::regmgmt::RegisterPressureManager;
use crate::naming::new_function_naming;
use rcc_frontend::ir::IrType;
use rcc_codegen::{AsmInst, Reg};

/// Helper to simulate a complete function call and verify parameter passing
fn simulate_call_and_verify(
    args: Vec<CallArg>,
    param_types: Vec<(rcc_common::TempId, IrType)>,
    expected_register_args: Vec<(usize, Reg)>,  // (param_index, expected_register)
    expected_stack_args: Vec<(usize, i16)>,      // (param_index, expected_offset)
) {
    let cc = CallingConvention::new();
    
    // === CALLER SIDE ===
    let mut caller_pm = RegisterPressureManager::new(0);
    caller_pm.init();
    let mut caller_naming = new_function_naming();
    
    // Setup arguments
    let setup_insts = cc.setup_call_args(&mut caller_pm, &mut caller_naming, args.clone(), None);
    
    // Count how many args go to stack
    // This needs to match the logic in setup_call_args
    let mut reg_slots_used = 0;
    let mut stack_words = 0;
    
    for arg in &args {
        match arg {
            CallArg::Scalar(_) => {
                if reg_slots_used < 4 {
                    reg_slots_used += 1;
                } else {
                    stack_words += 1;
                }
            }
            CallArg::FatPointer { .. } => {
                if reg_slots_used + 1 < 4 {  // Matches line 60 in calling_convention.rs
                    reg_slots_used += 2;
                } else {
                    stack_words += 2;
                    // Once we go to stack, everything else goes to stack
                    reg_slots_used = 4;
                }
            }
            CallArg::I64 { .. } => {
                if reg_slots_used + 4 <= 4 {
                    reg_slots_used += 4;
                } else {
                    stack_words += 4;
                    reg_slots_used = 4;
                }
            }
        }
    }
    
    // Verify stack stores match expected
    let store_count = setup_insts.iter()
        .filter(|i| matches!(i, AsmInst::Store(_, Reg::Sb, Reg::Sp)))
        .count();
    assert_eq!(store_count, stack_words as usize, 
               "Stack stores should match expected stack words");
    
    // === CALLEE SIDE ===
    let mut callee_pm = RegisterPressureManager::new(0);
    callee_pm.init();
    let mut callee_naming = new_function_naming();
    
    // Verify register parameters
    for (param_idx, expected_reg) in expected_register_args {
        let (load_insts, _dest, _bank_reg) = cc.load_param(param_idx, &param_types, &mut callee_pm, &mut callee_naming);
        
        // Should load from the expected register
        assert!(load_insts.iter().any(|i| matches!(i, AsmInst::Add(_, reg, Reg::R0) if *reg == expected_reg)),
                "Parameter {} should be loaded from {:?}", param_idx, expected_reg);
    }
    
    // Verify stack parameters
    for (param_idx, expected_offset) in expected_stack_args {
        let (load_insts, _dest, _bank_reg) = cc.load_param(param_idx, &param_types, &mut callee_pm, &mut callee_naming);
        
        // Debug: print what we actually got
        for inst in &load_insts {
            if let AsmInst::AddI(Reg::Sc, Reg::Fp, offset) = inst {
                eprintln!("Parameter {} actual offset: FP{}", param_idx, offset);
            }
        }
        
        // Should load from the expected stack offset
        assert!(load_insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sc, Reg::Fp, offset) if *offset == expected_offset)),
                "Parameter {} should be loaded from FP{}", param_idx, expected_offset);
    }
}

#[test]
fn test_all_scalars_end_to_end() {
    // Test with 6 scalar parameters: first 4 in registers, last 2 on stack
    let args = vec![
        CallArg::Scalar(Reg::T0),
        CallArg::Scalar(Reg::T1),
        CallArg::Scalar(Reg::T2),
        CallArg::Scalar(Reg::T3),
        CallArg::Scalar(Reg::T4),
        CallArg::Scalar(Reg::T5),
    ];
    
    let param_types = vec![
        (0, IrType::I16),
        (1, IrType::I16),
        (2, IrType::I16),
        (3, IrType::I16),
        (4, IrType::I16),
        (5, IrType::I16),
    ];
    
    simulate_call_and_verify(
        args,
        param_types,
        vec![
            (0, Reg::A0),
            (1, Reg::A1),
            (2, Reg::A2),
            (3, Reg::A3),
        ],
        vec![
            (4, -8),  // First stack param at FP-8
            (5, -9),  // Second stack param at FP-9
        ],
    );
}

#[test]
fn test_pure_fat_pointers_end_to_end() {
    // Test with 3 fat pointers: first 2 fill registers, third on stack
    let args = vec![
        CallArg::FatPointer { addr: Reg::T0, bank: Reg::T1 },
        CallArg::FatPointer { addr: Reg::T2, bank: Reg::T3 },
        CallArg::FatPointer { addr: Reg::T4, bank: Reg::T5 },
    ];
    
    let param_types = vec![
        (0, IrType::FatPtr(Box::new(IrType::I16))),
        (1, IrType::FatPtr(Box::new(IrType::I16))),
        (2, IrType::FatPtr(Box::new(IrType::I16))),
    ];
    
    simulate_call_and_verify(
        args,
        param_types,
        vec![
            (0, Reg::A0),  // First fat ptr address in A0 (bank in A1)
            (1, Reg::A2),  // Second fat ptr address in A2 (bank in A3)
        ],
        vec![
            (2, -8),  // Third fat ptr address at FP-8 (bank at FP-9)
        ],
    );
}

#[test]
fn test_mixed_types_end_to_end() {
    // Complex mix: scalar, fat ptr, scalar, scalar, fat ptr
    let args = vec![
        CallArg::Scalar(Reg::T0),                              // -> A0
        CallArg::FatPointer { addr: Reg::T1, bank: Reg::T2 },  // -> A1, A2
        CallArg::Scalar(Reg::T3),                              // -> A3
        CallArg::Scalar(Reg::T4),                              // -> stack
        CallArg::FatPointer { addr: Reg::T5, bank: Reg::T6 },  // -> stack
    ];
    
    let param_types = vec![
        (0, IrType::I16),
        (1, IrType::FatPtr(Box::new(IrType::I16))),
        (2, IrType::I16),
        (3, IrType::I16),
        (4, IrType::FatPtr(Box::new(IrType::I16))),
    ];
    
    simulate_call_and_verify(
        args,
        param_types,
        vec![
            (0, Reg::A0),
            (1, Reg::A1),  // Fat ptr address (bank in A2)
            (2, Reg::A3),
        ],
        vec![
            (3, -8),   // First stack scalar
            (4, -9),   // Fat ptr address (bank at -10)
        ],
    );
}

#[test]
fn test_partial_fat_pointer_in_registers() {
    // Test edge case: 3 scalars then fat pointer
    // In our implementation, fat pointers are atomic - they need 2 consecutive registers
    // So if only 1 register slot is left, the entire fat pointer goes to stack
    let args = vec![
        CallArg::Scalar(Reg::T0),                              // -> A0
        CallArg::Scalar(Reg::T1),                              // -> A1
        CallArg::Scalar(Reg::T2),                              // -> A2
        CallArg::FatPointer { addr: Reg::T3, bank: Reg::T4 },  // -> stack (needs 2 slots, only 1 left)
        CallArg::Scalar(Reg::T5),                              // -> A3
    ];
    
    let param_types = vec![
        (0, IrType::I16),
        (1, IrType::I16),
        (2, IrType::I16),
        (3, IrType::FatPtr(Box::new(IrType::I16))),
        (4, IrType::I16),
    ];
    
    // With the corrected behavior:
    // - First 3 scalars use A0-A2  
    // - Fat pointer needs 2 slots but only 1 is available, so it goes to stack
    // - Once we use stack, all remaining args must go to stack for consistency
    // - So the last scalar also goes to stack
    simulate_call_and_verify(
        args,
        param_types,
        vec![
            (0, Reg::A0),
            (1, Reg::A1),
            (2, Reg::A2),
        ],
        vec![
            (3, -8),  // Fat ptr address at FP-8 (bank at FP-9)
            (4, -10), // Last scalar at FP-10
        ],
    );
}

#[test]
fn test_empty_call() {
    // Test function with no parameters
    let args = vec![];
    let param_types = vec![];
    
    simulate_call_and_verify(
        args,
        param_types,
        vec![],  // No register params
        vec![],  // No stack params
    );
}

#[test]
fn test_single_scalar() {
    let args = vec![CallArg::Scalar(Reg::T0)];
    let param_types = vec![(0, IrType::I16)];
    
    simulate_call_and_verify(
        args,
        param_types,
        vec![(0, Reg::A0)],
        vec![],
    );
}

#[test]
fn test_single_fat_pointer() {
    let args = vec![CallArg::FatPointer { addr: Reg::T0, bank: Reg::T1 }];
    let param_types = vec![(0, IrType::FatPtr(Box::new(IrType::I16)))];
    
    simulate_call_and_verify(
        args,
        param_types,
        vec![(0, Reg::A0)],  // Address in A0 (bank in A1)
        vec![],
    );
}

#[test]
fn test_maximum_register_usage() {
    // Test that exactly fills all 4 registers
    let args = vec![
        CallArg::FatPointer { addr: Reg::T0, bank: Reg::T1 },  // A0-A1
        CallArg::FatPointer { addr: Reg::T2, bank: Reg::T3 },  // A2-A3
        CallArg::Scalar(Reg::T4),                              // Stack
    ];
    
    let param_types = vec![
        (0, IrType::FatPtr(Box::new(IrType::I16))),
        (1, IrType::FatPtr(Box::new(IrType::I16))),
        (2, IrType::I16),
    ];
    
    simulate_call_and_verify(
        args,
        param_types,
        vec![
            (0, Reg::A0),
            (1, Reg::A2),
        ],
        vec![
            (2, -8),  // First stack param
        ],
    );
}

#[test]
fn test_alternating_types() {
    // Test alternating scalar and fat pointer types
    let args = vec![
        CallArg::Scalar(Reg::T0),                              // A0
        CallArg::FatPointer { addr: Reg::T1, bank: Reg::T2 },  // A1-A2
        CallArg::Scalar(Reg::T3),                              // A3
        CallArg::FatPointer { addr: Reg::T4, bank: Reg::T5 },  // Stack
        CallArg::Scalar(Reg::T6),                              // Stack
        CallArg::FatPointer { addr: Reg::T7, bank: Reg::S0 },  // Stack
    ];
    
    let param_types = vec![
        (0, IrType::I16),
        (1, IrType::FatPtr(Box::new(IrType::I16))),
        (2, IrType::I16),
        (3, IrType::FatPtr(Box::new(IrType::I16))),
        (4, IrType::I16),
        (5, IrType::FatPtr(Box::new(IrType::I16))),
    ];
    
    simulate_call_and_verify(
        args,
        param_types,
        vec![
            (0, Reg::A0),
            (1, Reg::A1),
            (2, Reg::A3),
        ],
        vec![
            (3, -8),   // Fat ptr address
            (4, -10),  // Scalar (after fat ptr bank at -9)
            (5, -11),  // Fat ptr address (bank at -12)
        ],
    );
}

#[test]
fn test_fat_pointer_loading_both_parts() {
    // Test that we can load both parts of a fat pointer parameter
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    let mut naming = new_function_naming();
    
    let param_types = vec![
        (0, IrType::FatPtr(Box::new(IrType::I16))),
    ];
    
    // Load the fat pointer parameter (currently loads address part)
    let (insts, _addr_reg, _bank_reg) = cc.load_param(0, &param_types, &mut pm, &mut naming);
    
    // Should load address from A0
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(_, Reg::A0, Reg::R0))),
            "Fat pointer address should come from A0");
    
    // For the bank part, we'd need to either:
    // 1. Enhance load_param to return both registers for fat pointers
    // 2. Have a separate way to get the bank register (A1 in this case)
    // 3. Use load_fat_param (but it has issues as discussed)
    
    // This test highlights that we need a proper way to load both parts
    // of a fat pointer parameter
}

#[test]
fn test_stack_offset_correctness() {
    // Detailed verification of stack offset calculations
    let _args = vec![
        CallArg::Scalar(Reg::T0),                              // A0
        CallArg::Scalar(Reg::T1),                              // A1
        CallArg::FatPointer { addr: Reg::T2, bank: Reg::T3 },  // A2-A3
        CallArg::Scalar(Reg::T4),                              // Stack -8
        CallArg::FatPointer { addr: Reg::T5, bank: Reg::T6 },  // Stack -9, -10
        CallArg::Scalar(Reg::T7),                              // Stack -11
        CallArg::FatPointer { addr: Reg::S0, bank: Reg::S1 },  // Stack -12, -13
    ];
    
    let param_types = vec![
        (0, IrType::I16),
        (1, IrType::I16),
        (2, IrType::FatPtr(Box::new(IrType::I16))),
        (3, IrType::I16),
        (4, IrType::FatPtr(Box::new(IrType::I16))),
        (5, IrType::I16),
        (6, IrType::FatPtr(Box::new(IrType::I16))),
    ];
    
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    let mut naming = new_function_naming();
    
    // Verify each parameter loads from the correct location
    let expected_locations: Vec<(usize, Option<Reg>, Option<i16>)> = vec![
        (0, Some(Reg::A0), None),
        (1, Some(Reg::A1), None),
        (2, Some(Reg::A2), None),  // Fat ptr address (bank in A3)
        (3, None, Some(-8)),        // First stack param
        (4, None, Some(-9)),        // Fat ptr address (bank at -10)
        (5, None, Some(-11)),       // Scalar after fat ptr
        (6, None, Some(-12)),       // Fat ptr address (bank at -13)
    ];
    
    for (i, expected_reg, expected_offset) in expected_locations {
        let (insts, _, _) = cc.load_param(i, &param_types, &mut pm, &mut naming);
        
        if let Some(reg) = expected_reg {
            assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Add(_, r, Reg::R0) if *r == reg)),
                    "Param {} should load from {:?}", i, reg);
        }
        
        if let Some(offset) = expected_offset {
            assert!(insts.iter().any(|inst| matches!(inst, AsmInst::AddI(Reg::Sc, Reg::Fp, o) if *o == offset)),
                    "Param {} should load from FP{}", i, offset);
        }
    }
}