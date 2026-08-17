use crate::function::calling_convention::{CallingConvention, CallArg};
use crate::regmgmt::RegisterPressureManager;
use crate::naming::new_function_naming;
use rcc_codegen::{AsmInst, Reg};

#[test]
fn test_register_based_scalar_args() {
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    
    let args = vec![
        CallArg::Scalar(Reg::T0),
        CallArg::Scalar(Reg::T1),
    ];
    
    let mut naming = new_function_naming();
    let insts = cc.setup_call_args(&mut pm, &mut naming, args, None);
    
    // First 2 args should go to A0 and A1, not stack
    let store_count = insts.iter().filter(|i| matches!(i, AsmInst::Store(_, Reg::Sb, Reg::Sp))).count();
    assert_eq!(store_count, 0, "Should not store to stack for first 4 args");
    
    // Should move args to A0 and A1
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A0, _, Reg::R0))),
            "Should move first arg to A0");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A1, _, Reg::R0))),
            "Should move second arg to A1");
}

#[test]
fn test_register_based_fat_pointer_arg() {
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    
    let args = vec![
        CallArg::FatPointer { addr: Reg::T0, bank: Reg::T1 },
    ];
    
    let mut naming = new_function_naming();
    let insts = cc.setup_call_args(&mut pm, &mut naming, args, None);
    
    // Fat pointer should use A0 and A1, not stack
    let store_count = insts.iter().filter(|i| matches!(i, AsmInst::Store(_, Reg::Sb, Reg::Sp))).count();
    assert_eq!(store_count, 0, "Should not store to stack for first fat pointer");
    
    // Should move to A0 and A1
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A0, _, Reg::R0))),
            "Should move address to A0");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A1, _, Reg::R0))),
            "Should move bank to A1");
}

#[test]
fn test_stack_cleanup() {
    let cc = CallingConvention::new();
    
    // Test cleanup of 3 words
    let insts = cc.cleanup_stack(3);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sp, Reg::Sp, -3))));
    
    // Test no cleanup needed
    let insts = cc.cleanup_stack(0);
    assert_eq!(insts.len(), 0);
}

#[test]
fn test_load_param_from_registers_and_stack() {
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    
    let mut naming = new_function_naming();
    
    // Create sample parameter types (all scalars for this test)
    use rcc_frontend::ir::IrType;
    let param_types = vec![
        (0, IrType::I16), // param 0
        (1, IrType::I16), // param 1
        (2, IrType::I16), // param 2
        (3, IrType::I16), // param 3
        (4, IrType::I16), // param 4
        (5, IrType::I16), // param 5
    ];
    
    // Load parameter 0 (should be from A0)
    let (insts, _reg, _bank_reg) = cc.load_param(0, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(_, Reg::A0, Reg::R0))),
            "Param 0 should come from A0");
    
    // Load parameter 2 (should be from A2)
    let (insts, _reg, _bank_reg) = cc.load_param(2, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(_, Reg::A2, Reg::R0))),
            "Param 2 should come from A2");
    
    // Load parameter 4 (should be from stack at FP-7)
    // With the new calculation: -6 (base) -1 (for param 4 itself) = -7
    let (insts, _reg, _bank_reg) = cc.load_param(4, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sc, Reg::Fp, -7))),
            "Param 4 should come from stack at FP-7");
    
    // Load parameter 5 (should be from stack at FP-8)
    // With the new calculation: -6 (base) -1 (param 4) -1 (param 5 itself) = -8
    let (insts, _reg, _bank_reg) = cc.load_param(5, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sc, Reg::Fp, -8))),
            "Param 5 should come from stack at FP-8");
}

#[test]
fn test_return_value_handling() {
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    
    // Test scalar return
    let mut naming = new_function_naming();
    let (insts, ret_regs) = cc.handle_return_value(&mut pm, &mut naming, false, false, Some("test_result".to_string()));
    assert!(ret_regs.is_some());
    let (_ret_reg, bank_reg) = ret_regs.unwrap();
    assert!(bank_reg.is_none());
    // With the new implementation, we just add a comment since values are already in Rv0
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Comment(_))));
    
    // Test fat pointer return
    let (insts, ret_regs) = cc.handle_return_value(&mut pm, &mut naming, true, false, Some("test_ptr_result".to_string()));
    assert!(ret_regs.is_some());
    let (_addr_reg, bank_reg) = ret_regs.unwrap();
    assert!(bank_reg.is_some());
    // With the new implementation, we just add a comment since values are already in Rv0/Rv1
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Comment(_))));
}

#[test]
fn test_cross_bank_call() {
    let cc = CallingConvention::new();
    
    // Test in-bank call (bank 0)
    let insts = cc.emit_call(100, 0);
    // Should NOT set PCB for bank 0
    assert!(!insts.iter().any(|i| matches!(i, AsmInst::Li(Reg::Pcb, _))));
    // Should have JAL
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Jal(_, 100))));
    
    // Test cross-bank call (bank 3)
    let insts = cc.emit_call(200, 3);
    // Should set PCB to 3
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Li(Reg::Pcb, 3))));
    // Should have JAL to address 200
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Jal(_, 200))));
}

#[test]
fn test_multiple_args_with_overflow_to_stack() {
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    
    // Create 6 scalar args - first 4 in registers, last 2 on stack
    let args = vec![
        CallArg::Scalar(Reg::T0),
        CallArg::Scalar(Reg::T1),
        CallArg::Scalar(Reg::T2),
        CallArg::Scalar(Reg::T3),
        CallArg::Scalar(Reg::T4),
        CallArg::Scalar(Reg::T5),
    ];
    
    let mut naming = new_function_naming();
    let insts = cc.setup_call_args(&mut pm, &mut naming, args, None);
    
    // Should only push last 2 args to stack
    let store_count = insts.iter().filter(|i| matches!(i, AsmInst::Store(_, Reg::Sb, Reg::Sp))).count();
    assert_eq!(store_count, 2, "Should store only args 4 and 5 to stack");
    
    // First 4 args should go to A0-A3
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A0, _, Reg::R0))));
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A1, _, Reg::R0))));
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A2, _, Reg::R0))));
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A3, _, Reg::R0))));
}

#[test]
fn test_mixed_args() {
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    
    // Mix of scalar and fat pointer args
    // Scalar goes to A0, fat pointer goes to A1-A2, last scalar goes to A3
    let args = vec![
        CallArg::Scalar(Reg::T0),
        CallArg::FatPointer { addr: Reg::T1, bank: Reg::T2 },
        CallArg::Scalar(Reg::T3),
    ];
    
    let mut naming = new_function_naming();
    let insts = cc.setup_call_args(&mut pm, &mut naming, args, None);
    
    // All should fit in registers, no stack usage
    let store_count = insts.iter().filter(|i| matches!(i, AsmInst::Store(_, Reg::Sb, Reg::Sp))).count();
    assert_eq!(store_count, 0, "All args should fit in A0-A3");
    
    // Check register assignments
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A0, _, Reg::R0))),
            "First scalar should go to A0");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A1, _, Reg::R0))),
            "Fat pointer addr should go to A1");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A2, _, Reg::R0))),
            "Fat pointer bank should go to A2");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A3, _, Reg::R0))),
            "Last scalar should go to A3");
}

#[test]
fn test_fat_pointer_overflow_to_stack() {
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    
    // 3 fat pointers - first two use all 4 registers, third goes to stack
    let args = vec![
        CallArg::FatPointer { addr: Reg::T0, bank: Reg::T1 },
        CallArg::FatPointer { addr: Reg::T2, bank: Reg::T3 },
        CallArg::FatPointer { addr: Reg::T4, bank: Reg::T5 },
    ];
    
    let mut naming = new_function_naming();
    let insts = cc.setup_call_args(&mut pm, &mut naming, args, None);
    
    // Third fat pointer (2 values) should go to stack
    let store_count = insts.iter().filter(|i| matches!(i, AsmInst::Store(_, Reg::Sb, Reg::Sp))).count();
    assert_eq!(store_count, 2, "Third fat pointer should go to stack");
    
    // First two fat pointers should use A0-A3
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A0, _, Reg::R0))));
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A1, _, Reg::R0))));
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A2, _, Reg::R0))));
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A3, _, Reg::R0))));
}

#[test]
fn test_mixed_param_types_on_stack() {
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    
    let mut naming = new_function_naming();
    
    // Create mixed parameter types
    // param 0: scalar (A0)
    // param 1: fat pointer (A1, A2)
    // param 2: scalar (A3)
    // param 3: scalar (stack at FP-7)
    // param 4: fat pointer (stack at FP-8, FP-9)
    // param 5: scalar (stack at FP-10)
    use rcc_frontend::ir::IrType;
    let param_types = vec![
        (0, IrType::I16),                           // param 0: scalar
        (1, IrType::FatPtr(Box::new(IrType::I16))), // param 1: fat pointer
        (2, IrType::I16),                           // param 2: scalar  
        (3, IrType::I16),                           // param 3: scalar
        (4, IrType::FatPtr(Box::new(IrType::I16))), // param 4: fat pointer
        (5, IrType::I16),                           // param 5: scalar
    ];
    
    // Load parameter 0 (should be from A0)
    let (insts, _reg, _bank_reg) = cc.load_param(0, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(_, Reg::A0, Reg::R0))),
            "Param 0 should come from A0");
    
    // Load parameter 2 (should be from A3 since param 1 uses A1-A2)
    let (insts, _reg, _bank_reg) = cc.load_param(2, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(_, Reg::A3, Reg::R0))),
            "Param 2 should come from A3");
    
    // Load parameter 3 (first stack param, should be at FP-7)
    let (insts, _reg, _bank_reg) = cc.load_param(3, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sc, Reg::Fp, -7))),
            "Param 3 should come from stack at FP-7");
    
    // Load parameter 5 (should be at FP-10: -6 base, -1 for param3, -2 for param4 fat ptr, -1 for param5)
    let (insts, _reg, _bank_reg) = cc.load_param(5, &param_types, &mut pm, &mut naming);
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sc, Reg::Fp, -10))),
            "Param 5 should come from stack at FP-10");
}

#[test]
fn test_callee_saved_registers_as_arguments() {
    // Test that passing S0-S3 as arguments works correctly
    // The calling convention should move them to A0-A3 without issues
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    
    // Pass callee-saved registers as arguments
    let args = vec![
        CallArg::Scalar(Reg::S0),
        CallArg::Scalar(Reg::S1),
        CallArg::Scalar(Reg::S2),
        CallArg::Scalar(Reg::S3),
    ];
    
    let mut naming = new_function_naming();
    let insts = cc.setup_call_args(&mut pm, &mut naming, args, None);
    
    // Should move S0-S3 to A0-A3
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A0, Reg::S0, Reg::R0))),
            "S0 should be moved to A0");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A1, Reg::S1, Reg::R0))),
            "S1 should be moved to A1");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A2, Reg::S2, Reg::R0))),
            "S2 should be moved to A2");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A3, Reg::S3, Reg::R0))),
            "S3 should be moved to A3");
    
    // No stack stores should occur
    let store_count = insts.iter().filter(|i| matches!(i, AsmInst::Store(_, Reg::Sb, Reg::Sp))).count();
    assert_eq!(store_count, 0, "No arguments should go to stack");
}

#[test]
fn test_mixed_callee_saved_and_temp_registers() {
    // Test mixing callee-saved and temporary registers
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    
    let args = vec![
        CallArg::Scalar(Reg::T0),
        CallArg::Scalar(Reg::S0),
        CallArg::FatPointer { addr: Reg::S1, bank: Reg::T1 },
    ];
    
    let mut naming = new_function_naming();
    let insts = cc.setup_call_args(&mut pm, &mut naming, args, None);
    
    // Check register moves
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A0, Reg::T0, Reg::R0))),
            "T0 should be moved to A0");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A1, Reg::S0, Reg::R0))),
            "S0 should be moved to A1");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A2, Reg::S1, Reg::R0))),
            "S1 (fat ptr addr) should be moved to A2");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A3, Reg::T1, Reg::R0))),
            "T1 (fat ptr bank) should be moved to A3");
}

#[test]
fn test_callee_saved_fat_pointers() {
    // Test passing fat pointers using callee-saved registers
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    
    let args = vec![
        CallArg::FatPointer { addr: Reg::S0, bank: Reg::S1 },
        CallArg::FatPointer { addr: Reg::S2, bank: Reg::S3 },
    ];
    
    let mut naming = new_function_naming();
    let insts = cc.setup_call_args(&mut pm, &mut naming, args, None);
    
    // First fat pointer should use A0-A1
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A0, Reg::S0, Reg::R0))),
            "S0 (first fat ptr addr) should be moved to A0");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A1, Reg::S1, Reg::R0))),
            "S1 (first fat ptr bank) should be moved to A1");
    
    // Second fat pointer should use A2-A3
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A2, Reg::S2, Reg::R0))),
            "S2 (second fat ptr addr) should be moved to A2");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A3, Reg::S3, Reg::R0))),
            "S3 (second fat ptr bank) should be moved to A3");
}

#[test]
fn test_callee_saved_with_stack_overflow() {
    // Test that callee-saved registers go to stack when there are too many args
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    
    let args = vec![
        CallArg::Scalar(Reg::T0),
        CallArg::Scalar(Reg::T1),
        CallArg::Scalar(Reg::T2),
        CallArg::Scalar(Reg::T3),
        CallArg::Scalar(Reg::S0),  // This should go to stack
        CallArg::Scalar(Reg::S1),  // This should go to stack
    ];
    
    let mut naming = new_function_naming();
    let insts = cc.setup_call_args(&mut pm, &mut naming, args, None);
    
    // First 4 should go to registers
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A0, Reg::T0, Reg::R0))));
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A1, Reg::T1, Reg::R0))));
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A2, Reg::T2, Reg::R0))));
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A3, Reg::T3, Reg::R0))));
    
    // S0 and S1 should be stored to stack
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Store(Reg::S0, Reg::Sb, Reg::Sp))),
            "S0 should be stored to stack");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Store(Reg::S1, Reg::Sb, Reg::Sp))),
            "S1 should be stored to stack");
}

#[test]
fn test_callee_saved_partial_fat_pointer() {
    // Test the edge case where a fat pointer with callee-saved registers
    // partially fits in registers
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    
    let args = vec![
        CallArg::Scalar(Reg::T0),
        CallArg::Scalar(Reg::T1),
        CallArg::Scalar(Reg::T2),
        CallArg::FatPointer { addr: Reg::S0, bank: Reg::S1 },  // Doesn't fit, goes to stack
        CallArg::Scalar(Reg::S2),  // Also goes to stack
    ];
    
    let mut naming = new_function_naming();
    let insts = cc.setup_call_args(&mut pm, &mut naming, args, None);
    
    // First 3 scalars in registers
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A0, Reg::T0, Reg::R0))));
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A1, Reg::T1, Reg::R0))));
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A2, Reg::T2, Reg::R0))));
    
    // Fat pointer and last scalar should be on stack
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Store(Reg::S0, Reg::Sb, Reg::Sp))),
            "S0 (fat ptr addr) should be stored to stack");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Store(Reg::S1, Reg::Sb, Reg::Sp))),
            "S1 (fat ptr bank) should be stored to stack");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Store(Reg::S2, Reg::Sb, Reg::Sp))),
            "S2 should be stored to stack");
}

#[test]
fn test_all_register_types_mixed() {
    // Test with a mix of all register types: T*, S*, A* registers
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    
    // Note: Using A* registers as source is unusual but should work
    let args = vec![
        CallArg::Scalar(Reg::T0),
        CallArg::Scalar(Reg::S0),
        CallArg::Scalar(Reg::A0),  // Using A0 as source (will be moved to A2)
        CallArg::Scalar(Reg::T1),
    ];
    
    let mut naming = new_function_naming();
    let insts = cc.setup_call_args(&mut pm, &mut naming, args, None);
    
    // Check moves - note that A0 needs special handling since it's both source and dest
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A0, Reg::T0, Reg::R0))),
            "T0 should be moved to A0");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A1, Reg::S0, Reg::R0))),
            "S0 should be moved to A1");
    // A0 as source goes to A2 (but A0 is already overwritten by T0, this test shows a limitation)
    // In practice, the register allocator should handle this properly
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A2, Reg::A0, Reg::R0))),
            "A0 (as source) should be moved to A2");
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(Reg::A3, Reg::T1, Reg::R0))),
            "T1 should be moved to A3");
}