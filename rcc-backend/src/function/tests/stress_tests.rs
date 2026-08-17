//! Stress tests for function internals
//! 
//! These tests have access to internal modules for thorough testing

use super::super::calling_convention::{CallingConvention, CallArg};
use super::super::internal::FunctionLowering;
use crate::regmgmt::RegisterPressureManager;
use crate::naming::new_function_naming;
use rcc_codegen::{AsmInst, Reg};

// ========================================================================
// CALLING CONVENTION STRESS TESTS
// ========================================================================

#[test]
fn stress_test_massive_argument_list() {
    // Test with 50+ arguments of mixed types
    let cc = CallingConvention::new();
    let mut pm = RegisterPressureManager::new(0);
    pm.init();
    
    let mut args = Vec::new();
    let mut expected_stack_words = 0;
    let mut register_slots_used = 0;
    
    for i in 0..50 {
        if i % 3 == 0 {
            // Every third arg is a fat pointer
            args.push(CallArg::FatPointer { 
                addr: Reg::A0,
                bank: Reg::A1
            });
            
            // Check if this goes to registers or stack
            if register_slots_used + 2 <= 4 {
                // Fits in registers
                register_slots_used += 2;
            } else {
                // Goes to stack
                expected_stack_words += 2;
            }
        } else {
            args.push(CallArg::Scalar(Reg::A0));
            
            // Check if this goes to registers or stack
            if register_slots_used < 4 {
                // Fits in registers
                register_slots_used += 1;
            } else {
                // Goes to stack
                expected_stack_words += 1;
            }
        }
    }
    let mut naming = new_function_naming();
    let insts = cc.setup_call_args(&mut pm, &mut naming, args, None);
    
    // Count stores to verify all args pushed
    let store_count = insts.iter()
        .filter(|i| matches!(i, AsmInst::Store(_, Reg::Sb, Reg::Sp)))
        .count();
    assert_eq!(store_count, expected_stack_words as usize);
    
    // Verify cleanup
    let cleanup = cc.cleanup_stack(expected_stack_words);
    assert!(cleanup.iter().any(|i| 
        matches!(i, AsmInst::AddI(Reg::Sp, Reg::Sp, n) if *n == -expected_stack_words)
    ));
}

#[test]
fn stress_test_nested_calls() {
    // Simulate deeply nested function calls
    let cc = CallingConvention::new();
    
    // Track stack depth
    let mut stack_adjustments = Vec::new();
    
    for depth in 0..10 {
        // Setup args for this call level
        let args = vec![
            CallArg::Scalar(Reg::A0),
            CallArg::FatPointer { addr: Reg::A1, bank: Reg::A2 },
        ];
        
        let mut pm = RegisterPressureManager::new(0);
        pm.init();
        let mut naming = new_function_naming();
        let setup = cc.setup_call_args(&mut pm, &mut naming, args, None);
        
        // Make the call
        let call = cc.emit_call(100 + depth as u16, depth as u16 % 4);
        
        // Handle return
        let result_name = Some(format!("result_{}", depth));
        let (_insts, ret_regs) = cc.handle_return_value(&mut pm, &mut naming, depth % 2 == 0, false, result_name);
        let _ret_regs = ret_regs; // Just to use it
        
        // Cleanup
        let cleanup = cc.cleanup_stack(3); // 1 scalar + 2 for fat pointer
        
        stack_adjustments.push((setup, call, cleanup));
    }
    
    // Verify each level has proper setup/cleanup
    for (setup, call, cleanup) in stack_adjustments {
        assert!(!setup.is_empty());
        assert!(!call.is_empty());
        assert!(!cleanup.is_empty());
    }
}

// ========================================================================
// FUNCTION LOWERING STRESS TESTS
// ========================================================================

#[test]
fn stress_test_huge_stack_frame() {
    // Test with massive local allocation
    let mut func = FunctionLowering::new();
    let insts = func.emit_prologue(1000);
    
    // Stack bank is initialized in crt0.asm, not in function prologue
    
    // Should allocate 1000 slots
    assert!(insts.iter().any(|i| matches!(i, AsmInst::AddI(Reg::Sp, Reg::Sp, 1000))));
}

#[test]
fn stress_test_many_local_accesses() {
    // Test accessing many different local variables
    let mut func = FunctionLowering::new();
    func.pressure_manager.init();
    
    // Access 100 different locals
    for i in 0..100 {
        let _addr = func.get_local_addr(i);
        
        // Load from local
        let load_insts = func.load_local(i, Reg::A0);
        assert!(load_insts.iter().any(|inst| 
            matches!(inst, AsmInst::Load(Reg::A0, Reg::Sb, _))
        ));
        
        // Store to local
        let store_insts = func.store_local(i, Reg::A1);
        assert!(store_insts.iter().any(|inst| 
            matches!(inst, AsmInst::Store(Reg::A1, Reg::Sb, _))
        ));
    }
}

#[test]
fn stress_test_complex_return_scenarios() {
    let mut func = FunctionLowering::new();
    func.pressure_manager.init();
    
    // Test 1: Return with value already in R3
    let insts1 = func.emit_return(Some((Reg::Rv0, None)));
    // Should not generate move to R3
    let moves_to_r3 = insts1.iter()
        .filter(|i| matches!(i, AsmInst::Add(Reg::Rv0, _, Reg::R0)))
        .count();
    assert_eq!(moves_to_r3, 0, "Should not move R3 to R3");
    
    // Test 2: Fat pointer already in R3/R4
    let mut func2 = FunctionLowering::new();
    func2.pressure_manager.init();
    let insts2 = func2.emit_return(Some((Reg::Rv0, Some(Reg::Rv1))));
    let moves = insts2.iter()
        .filter(|i| matches!(i, AsmInst::Add(Reg::Rv0, _, Reg::R0)) ||
                    matches!(i, AsmInst::Add(Reg::Rv1, _, Reg::R0)))
        .count();
    assert_eq!(moves, 0, "Should not move R3/R4 to themselves");
    
    // Test 3: Values in high registers
    let mut func3 = FunctionLowering::new();
    func3.pressure_manager.init();
    let insts3 = func3.emit_return(Some((Reg::X2, Some(Reg::X1))));
    assert!(insts3.iter().any(|i| matches!(i, AsmInst::Add(Reg::Rv0, Reg::X2, Reg::R0))));
    assert!(insts3.iter().any(|i| matches!(i, AsmInst::Add(Reg::Rv1, Reg::X1, Reg::R0))));
}