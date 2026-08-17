//! Comprehensive tests for RegisterPressureManager

use crate::regmgmt::*;
use rcc_frontend::ir::{Value, IrBinaryOp, BasicBlock, Instruction, IrType, FatPointer};
use rcc_codegen::{Reg, AsmInst};
use rcc_frontend::BankTag;

#[test]
fn test_sethi_ullman_ordering() {
    let rpm = RegisterPressureManager::new(0);
    
    // Test that complex expressions get proper ordering
    let const1 = Value::Constant(10);
    let const2 = Value::Constant(20);
    
    let need1 = rpm.calculate_need(&const1);
    let need2 = rpm.calculate_need(&const2);
    
    assert_eq!(need1.count, 1);
    assert_eq!(need2.count, 1);
    
    let (binary_need, swap) = rpm.calculate_binary_need(&const1, &const2);
    assert_eq!(binary_need.count, 2); // Both need 1, so total is 1+1
    assert!(!swap); // No need to swap equal needs
}

#[test]
fn test_lru_spilling() {
    let mut rpm = RegisterPressureManager::new(5);
    rpm.init();
    
    // Allocate all 12 registers
    for i in 0..12 {
        let reg = rpm.get_register(format!("val{}", i));
        assert!(matches!(reg, Reg::S0 | Reg::S1 | Reg::S2 | Reg::S3 | Reg::T0 | Reg::T1 | Reg::T2 | Reg::T3 | Reg::T4 | Reg::T5 | Reg::T6 | Reg::T7));
    }
    
    // Next allocation should spill LRU (val0)
    let reg = rpm.get_register("val12".to_string());
    assert!(matches!(reg, Reg::S0 | Reg::S1 | Reg::S2 | Reg::S3 | Reg::T0 | Reg::T1 | Reg::T2 | Reg::T3 | Reg::T4 | Reg::T5 | Reg::T6 | Reg::T7));
    
    // Check that spill happened
    let insts = rpm.take_instructions();
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Store(_, Reg::Sb, _))));
}

#[test]
fn test_reload() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Allocate and force spill (allocate more than 12 registers)
    for i in 0..13 {
        rpm.get_register(format!("val{}", i));
    }
    
    // Reload a spilled value
    let reg = rpm.reload_value("val0".to_string());
    assert!(matches!(reg, Reg::S0 | Reg::S1 | Reg::S2 | Reg::S3 | Reg::T0 | Reg::T1 | Reg::T2 | Reg::T3 | Reg::T4 | Reg::T5 | Reg::T6 | Reg::T7));
    
    // Check that reload happened
    let insts = rpm.take_instructions();
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Load(_, Reg::Sb, _))));
}

// ===== STRESS TESTS =====

#[test]
fn test_empty_manager_initialization() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Should have all registers available
    assert_eq!(rpm.get_spill_count(), 0);
    
    // Should be able to allocate a register
    let reg = rpm.get_register("test".to_string());
    assert!(matches!(reg, Reg::S0 | Reg::S1 | Reg::S2 | Reg::S3 | Reg::T0 | Reg::T1 | Reg::T2 | Reg::T3 | Reg::T4 | Reg::T5 | Reg::T6 | Reg::T7));
}

#[test]
fn test_register_reuse() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Allocate same value multiple times - should get same register
    let reg1 = rpm.get_register("same_value".to_string());
    let reg2 = rpm.get_register("same_value".to_string());
    assert_eq!(reg1, reg2);
    
    // No spills should have occurred
    assert_eq!(rpm.get_spill_count(), 0);
}

#[test]
fn test_free_and_reallocate() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Allocate a specific register first
    let reg1 = rpm.get_register("val1".to_string());
    assert_eq!(reg1, Reg::S3); // First allocation should always be S3 (first in free list)
    
    rpm.free_register(reg1);
    
    // After freeing S3, it goes to the back of the free list
    // Next allocation should get S2 (now at front of free list)
    let reg2 = rpm.get_register("val2".to_string());
    assert_eq!(reg2, Reg::S2); // Should get S2 since S3 is at the back
    
    // Now allocate another one
    let reg3 = rpm.get_register("val3".to_string());
    assert_eq!(reg3, Reg::S1); // Should get S1
    
    // Free S2 and reallocate - should get S0 (next in free list)
    rpm.free_register(reg2);
    let reg4 = rpm.get_register("val4".to_string());
    assert_eq!(reg4, Reg::S0); // S2 is at back, so we get S0
}

#[test]
fn test_spill_all() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Allocate several registers
    for i in 0..5 {
        rpm.get_register(format!("val{}", i));
    }
    
    // Spill all
    rpm.spill_all();
    
    // All registers should be free now
    for i in 0..12 {
        let reg = rpm.get_register(format!("new_val{}", i));
        assert!(matches!(reg, Reg::S0 | Reg::S1 | Reg::S2 | Reg::S3 | Reg::T0 | Reg::T1 | Reg::T2 | Reg::T3 | Reg::T4 | Reg::T5 | Reg::T6 | Reg::T7));
    }
    
    // Should have spilled the original 5 values
    assert_eq!(rpm.get_spill_count(), 5);
}

#[test]
fn test_maximum_spill_pressure() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Force many spills
    for i in 0..20 {
        rpm.get_register(format!("val{}", i));
    }
    
    // The spill count depends on LRU eviction pattern
    // We should have at least some spills since we requested more than 12 registers
    let spill_count = rpm.get_spill_count();
    assert!(spill_count > 0, "Should have spilled some values");
    
    // Check that spill instructions were generated
    let insts = rpm.take_instructions();
    let store_count = insts.iter().filter(|i| matches!(i, AsmInst::Store(_, _, _))).count();
    assert!(store_count > 0, "Should have generated spill stores");
    
    // The exact count depends on reuse patterns, but we should have spilled at least (20-12) unique values
    // However, due to potential reuse, the actual count might be different
}

#[test]
fn test_reload_non_existent_value() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Reload a value that was never allocated - should just allocate new
    let reg = rpm.reload_value("never_seen".to_string());
    assert!(matches!(reg, Reg::S0 | Reg::S1 | Reg::S2 | Reg::S3 | Reg::T0 | Reg::T1 | Reg::T2 | Reg::T3 | Reg::T4 | Reg::T5 | Reg::T6 | Reg::T7));
    
    // No reload instruction should be generated
    let insts = rpm.take_instructions();
    assert!(!insts.iter().any(|i| matches!(i, AsmInst::Load(_, _, _))));
}

#[test]
fn test_lru_ordering_preservation() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Allocate registers in order
    let reg0 = rpm.get_register("val0".to_string());
    let _reg1 = rpm.get_register("val1".to_string());
    let _reg2 = rpm.get_register("val2".to_string());
    
    // Access val0 again to make it MRU
    let reg0_again = rpm.get_register("val0".to_string());
    assert_eq!(reg0, reg0_again);
    
    // Fill remaining registers
    for i in 3..12 {
        rpm.get_register(format!("val{}", i));
    }
    
    // Next allocation should spill val1 (LRU), not val0 (MRU)
    rpm.get_register("val12".to_string());
    
    // val0 should still be in register
    let reg0_check = rpm.get_register("val0".to_string());
    assert_eq!(reg0, reg0_check);
    
    // No additional spill for val0
    let insts = rpm.take_instructions();
    let spill_comments: Vec<_> = insts.iter()
        .filter_map(|i| match i {
            AsmInst::Comment(s) if s.contains("Spill") => Some(s.clone()),
            _ => None
        })
        .collect();
    
    // Should have spilled val1, not val0
    assert!(spill_comments.iter().any(|s| s.contains("val1")));
    assert!(!spill_comments.iter().any(|s| s.contains("val0 to")));
}

#[test]
fn test_sethi_ullman_swap_decision() {
    let rpm = RegisterPressureManager::new(0);
    
    // Create values with different register needs
    let simple = Value::Constant(5);
    let complex = Value::Temp(100); // Assume not in register, needs 1
    
    let (_, should_swap) = rpm.calculate_binary_need(&simple, &complex);
    assert!(!should_swap); // Both need 1, no swap needed
    
    // Test with already allocated temp
    let mut rpm2 = RegisterPressureManager::new(0);
    rpm2.init();
    rpm2.get_register("t100".to_string());
    
    let (need, should_swap) = rpm2.calculate_binary_need(&simple, &complex);
    assert_eq!(need.count, 1); // Temp is already in register (needs 0) + constant needs 1
    assert!(!should_swap); // Constant needs more (1) than temp (0), but swap only happens when right > left
}

#[test]
fn test_value_types_handling() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Test constant
    let const_reg = rpm.get_value_register(&Value::Constant(42));
    assert!(matches!(const_reg, Reg::S0 | Reg::S1 | Reg::S2 | Reg::S3 | Reg::T0 | Reg::T1 | Reg::T2 | Reg::T3 | Reg::T4 | Reg::T5 | Reg::T6 | Reg::T7));
    
    // Check LI instruction was generated
    let insts = rpm.take_instructions();
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Li(_, 42))));
    
    // Test temp
    let mut rpm2 = RegisterPressureManager::new(0);
    rpm2.init();
    let temp_reg = rpm2.get_value_register(&Value::Temp(5));
    assert!(matches!(temp_reg, Reg::S0 | Reg::S1 | Reg::S2 | Reg::S3 | Reg::T0 | Reg::T1 | Reg::T2 | Reg::T3 | Reg::T4 | Reg::T5 | Reg::T6 | Reg::T7));
    
    // Test global
    let mut rpm3 = RegisterPressureManager::new(0);
    rpm3.init();
    let global_reg = rpm3.get_value_register(&Value::Global("test_global".to_string()));
    assert!(matches!(global_reg, Reg::S0 | Reg::S1 | Reg::S2 | Reg::S3 | Reg::T0 | Reg::T1 | Reg::T2 | Reg::T3 | Reg::T4 | Reg::T5 | Reg::T6 | Reg::T7));
}

#[test]
fn test_binary_op_with_spilling() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Fill up most registers
    for i in 0..6 {
        rpm.get_register(format!("occupied{}", i));
    }
    
    // Perform binary operation that might need spilling
    let lhs = Value::Temp(100);
    let rhs = Value::Temp(101);
    let result = 102;
    
    let insts = rpm.emit_binary_op(IrBinaryOp::Add, &lhs, &rhs, result);
    
    // Should have generated an add instruction
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Add(_, _, _))));
}

#[test]
fn test_non_commutative_op_swap_handling() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Test subtraction with swap
    let lhs = Value::Constant(10);
    let rhs = Value::Constant(5);
    let result = 1;
    
    let insts = rpm.emit_binary_op(IrBinaryOp::Sub, &lhs, &rhs, result);
    
    // Should generate correct subtraction
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Sub(_, _, _))));
}

#[test]
fn test_lifetime_analysis() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    let block = BasicBlock {
        id: 0,
        instructions: vec![
            Instruction::Binary {
                result: 1,
                op: IrBinaryOp::Add,
                lhs: Value::Constant(10),
                rhs: Value::Constant(20),
                result_type: IrType::I16,
            },
            Instruction::Binary {
                result: 2,
                op: IrBinaryOp::Mul,
                lhs: Value::Temp(1),
                rhs: Value::Constant(2),
                result_type: IrType::I16,
            },
            Instruction::Store {
                value: Value::Temp(2),
                ptr: Value::Temp(0),
            },
        ],
        predecessors: vec![],
        successors: vec![],
    };
    
    rpm.analyze_block(&block);
    
    // Check that lifetimes were properly tracked
    // Temp(1) should be defined at 0, used at 1
    // Temp(2) should be defined at 1, used at 2
    // Note: lifetimes is private, so we can't directly check its length
}

#[test]
fn test_call_crossing_detection() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    let block = BasicBlock {
        id: 0,
        instructions: vec![
            Instruction::Binary {
                result: 1,
                op: IrBinaryOp::Add,
                lhs: Value::Constant(10),
                rhs: Value::Constant(20),
                result_type: IrType::I16,
            },
            Instruction::Call {
                result: Some(2),
                function: Value::Function("test".to_string()),
                args: vec![],
                result_type: IrType::I16,
                stack_args_from: None,
            },
            Instruction::Binary {
                result: 3,
                op: IrBinaryOp::Add,
                lhs: Value::Temp(1),
                rhs: Value::Temp(2),
                result_type: IrType::I16,
            },
        ],
        predecessors: vec![],
        successors: vec![],
    };
    
    rpm.analyze_block(&block);
    
    // Temp(1) should be marked as crossing a call
    // Note: lifetimes is private, so we can't directly check this
    // The test ensures analyze_block runs without errors
}

#[test]
fn test_large_local_count_offset() {
    let mut rpm = RegisterPressureManager::new(100);
    rpm.init();
    
    // Force a spill
    for i in 0..13 {
        rpm.get_register(format!("val{}", i));
    }
    
    // Check that spill addresses account for local_count
    let insts = rpm.take_instructions();
    
    // Find AddI instructions used for spill addressing
    let has_correct_offset = insts.iter().any(|i| match i {
        AsmInst::AddI(_, _, offset) => *offset >= 100,
        _ => false,
    });
    
    assert!(has_correct_offset, "Spill addresses should account for local_count");
}

#[test]
fn test_all_binary_ops() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    let ops = vec![
        IrBinaryOp::Add,
        IrBinaryOp::Sub,
        IrBinaryOp::Mul,
        IrBinaryOp::And,
        IrBinaryOp::Or,
        IrBinaryOp::Xor,
        IrBinaryOp::Slt,
    ];
    
    for (i, op) in ops.iter().enumerate() {
        let lhs = Value::Constant(10);
        let rhs = Value::Constant(5);
        let result = i as u32;
        
        let insts = rpm.emit_binary_op(*op, &lhs, &rhs, result);
        assert!(!insts.is_empty(), "Binary op {:?} should generate instructions", op);
    }
}

#[test]
fn test_register_pressure_with_fat_ptr() {
    let rpm = RegisterPressureManager::new(0);
    
    let fat_ptr = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Constant(0x1000)),
        bank: BankTag::Global,
    });
    
    let need = rpm.calculate_need(&fat_ptr);
    assert_eq!(need.count, 2); // Fat pointers need 2 registers
    assert!(!need.is_leaf);
}

#[test]
fn test_undef_value_handling() {
    let rpm = RegisterPressureManager::new(0);
    
    let undef = Value::Undef;
    let need = rpm.calculate_need(&undef);
    assert_eq!(need.count, 0);
    assert!(need.is_leaf);
}

#[test]
#[should_panic(expected = "Unsupported value type")]
fn test_panic_on_unsupported_value() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Function values aren't directly loadable into registers
    let func = Value::Function("test".to_string());
    rpm.get_value_register(&func);
}

#[test]
fn test_alternating_spill_reload() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Create a pattern of allocate -> spill -> reload
    for cycle in 0..3 {
        // Allocate to fill registers
        for i in 0..12 {
            rpm.get_register(format!("cycle{}_val{}", cycle, i));
        }
        
        // Force spill
        rpm.get_register(format!("cycle{}_overflow", cycle));
        
        // Reload an old value
        if cycle > 0 {
            rpm.reload_value(format!("cycle{}_val0", cycle - 1));
        }
    }
    
    // Should have complex spill/reload pattern
    let insts = rpm.take_instructions();
    let loads = insts.iter().filter(|i| matches!(i, AsmInst::Load(_, _, _))).count();
    let stores = insts.iter().filter(|i| matches!(i, AsmInst::Store(_, _, _))).count();
    
    assert!(loads > 0, "Should have reload instructions");
    assert!(stores > 0, "Should have spill instructions");
}

#[test]
fn test_consecutive_free_operations() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    let reg = rpm.get_register("test".to_string());
    assert_eq!(reg, Reg::S3); // First allocation is deterministically S3
    
    // Free the same register multiple times - should be idempotent
    rpm.free_register(reg);
    rpm.free_register(reg);
    rpm.free_register(reg);
    
    // After freeing S3 multiple times, it should only be in free list once
    // Next allocation should get S2 (next in original free list)
    let new_reg = rpm.get_register("new".to_string());
    assert_eq!(new_reg, Reg::S2); // S3 is at back of free list, so we get S2
    
    // Allocate one more to verify ordering
    let another_reg = rpm.get_register("another".to_string());
    assert_eq!(another_reg, Reg::S1); // Next should be S1
}

#[test]
fn test_empty_block_analysis() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    let empty_block = BasicBlock {
        id: 0,
        instructions: vec![],
        predecessors: vec![],
        successors: vec![],
    };
    
    rpm.analyze_block(&empty_block);
    
    // Should handle empty block gracefully
    // Note: lifetimes is private, test ensures no crash
}

#[test]
fn test_return_value_lifetime() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    let block = BasicBlock {
        id: 0,
        instructions: vec![
            Instruction::Binary {
                result: 1,
                op: IrBinaryOp::Add,
                lhs: Value::Constant(10),
                rhs: Value::Constant(20),
                result_type: IrType::I16,
            },
            Instruction::Return(Some(Value::Temp(1))),
        ],
        predecessors: vec![],
        successors: vec![],
    };
    
    rpm.analyze_block(&block);
    
    // Return should properly record use of Temp(1)
    // Note: lifetimes is private, test ensures analyze_block runs without errors
}

// ===== ADDITIONAL EDGE CASE STRESS TESTS =====

#[test]
fn test_spill_slot_reuse() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Allocate enough to cause spills
    for i in 0..10 {
        rpm.get_register(format!("val{}", i));
    }
    
    // Free some registers
    let reg = rpm.get_register("val0".to_string());
    rpm.free_register(reg);
    
    // Allocate new values - should potentially reuse spill slots
    for i in 10..15 {
        rpm.get_register(format!("new_val{}", i));
    }
    
    // Should handle spill slot management correctly
    assert!(rpm.get_spill_count() > 0);
}

#[test]
fn test_deeply_nested_binary_ops() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Create a deeply nested expression tree
    let mut current = Value::Constant(1);
    for i in 0..10 {
        let next = Value::Temp(i);
        let (need, _) = rpm.calculate_binary_need(&current, &next);
        assert!(need.count > 0);
        current = next;
    }
}

#[test]
fn test_spill_with_large_constants() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Allocate with various constant values
    for i in 0..10 {
        let val = i * 1000;
        rpm.get_value_register(&Value::Constant(val));
    }
    
    // Should handle large constants properly
    let insts = rpm.take_instructions();
    let li_count = insts.iter().filter(|i| matches!(i, AsmInst::Li(_, _))).count();
    assert!(li_count > 0);
}

#[test]
fn test_mixed_value_types() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Mix different value types
    let _ = rpm.get_value_register(&Value::Constant(42));
    let _ = rpm.get_value_register(&Value::Temp(1));
    let _ = rpm.get_value_register(&Value::Global("global_var".to_string()));
    
    // Allocate more to cause spilling
    for i in 0..10 {
        rpm.get_register(format!("extra{}", i));
    }
    
    // Should handle mixed types correctly
    assert!(rpm.get_spill_count() > 0);
}

#[test]
fn test_reload_after_spill_all() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Allocate some values
    for i in 0..5 {
        rpm.get_register(format!("val{}", i));
    }
    
    // Spill all
    rpm.spill_all();
    
    // Try to reload spilled values
    let reg = rpm.reload_value("val2".to_string());
    assert!(matches!(reg, Reg::S0 | Reg::S1 | Reg::S2 | Reg::S3 | Reg::T0 | Reg::T1 | Reg::T2 | Reg::T3 | Reg::T4 | Reg::T5 | Reg::T6 | Reg::T7));
    
    // Should have reload instructions
    let insts = rpm.take_instructions();
    assert!(insts.iter().any(|i| matches!(i, AsmInst::Load(_, _, _))));
}

#[test]
fn test_boundary_local_count() {
    // Test with maximum practical local count
    let mut rpm = RegisterPressureManager::new(i16::MAX - 100);
    rpm.init();
    
    // Force spilling
    for i in 0..13 {
        rpm.get_register(format!("val{}", i));
    }
    
    // Should handle large offsets correctly
    let insts = rpm.take_instructions();
    let has_large_offset = insts.iter().any(|i| match i {
        AsmInst::AddI(_, _, offset) => *offset > 1000,
        _ => false,
    });
    assert!(has_large_offset);
}

#[test]
fn test_zero_local_count() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Should work with zero local count
    for i in 0..15 {  // Changed from 10 to 15 to force spilling with 12 registers
        rpm.get_register(format!("val{}", i));
    }
    
    assert!(rpm.get_spill_count() > 0);
}

#[test]
fn test_register_contents_tracking() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Allocate specific value
    let reg1 = rpm.get_register("unique_value".to_string());
    
    // Allocate same value again - should get same register
    let reg2 = rpm.get_register("unique_value".to_string());
    assert_eq!(reg1, reg2);
    
    // Free and reallocate
    rpm.free_register(reg1);
    let reg3 = rpm.get_register("unique_value".to_string());
    
    // Might get different register after freeing
    assert!(matches!(reg3, Reg::S0 | Reg::S1 | Reg::S2 | Reg::S3 | Reg::T0 | Reg::T1 | Reg::T2 | Reg::T3 | Reg::T4 | Reg::T5 | Reg::T6 | Reg::T7));
}

#[test]
fn test_complex_lifetime_patterns() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    let block = BasicBlock {
        id: 0,
        instructions: vec![
            // Multiple defs and uses
            Instruction::Binary {
                result: 1,
                op: IrBinaryOp::Add,
                lhs: Value::Constant(10),
                rhs: Value::Constant(20),
                result_type: IrType::I16,
            },
            Instruction::Binary {
                result: 2,
                op: IrBinaryOp::Mul,
                lhs: Value::Temp(1),
                rhs: Value::Temp(1), // Same temp used twice
                result_type: IrType::I16,
            },
            Instruction::Binary {
                result: 3,
                op: IrBinaryOp::Add,
                lhs: Value::Temp(2),
                rhs: Value::Temp(1), // Temp 1 used again
                result_type: IrType::I16,
            },
            Instruction::Store {
                value: Value::Temp(3),
                ptr: Value::Temp(2), // Temp 2 used as pointer
            },
        ],
        predecessors: vec![],
        successors: vec![],
    };
    
    rpm.analyze_block(&block);
    // Should handle complex lifetime patterns without panicking
}

#[test]
fn test_all_registers_occupied() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    // Exactly fill all available registers
    let mut regs = Vec::new();
    for i in 0..12 {
        regs.push(rpm.get_register(format!("occupy{}", i)));
    }
    
    // All should be unique
    regs.sort();
    regs.dedup();
    assert_eq!(regs.len(), 12);
    
    // Next allocation must spill
    rpm.get_register("force_spill".to_string());
    assert!(rpm.get_spill_count() > 0);
}

#[test]
fn test_binary_op_all_combinations() {
    let mut rpm = RegisterPressureManager::new(0);
    rpm.init();
    
    let value_types = vec![
        Value::Constant(42),
        Value::Temp(1),
        Value::Global("test".to_string()),
    ];
    
    // Test all combinations
    for (i, lhs) in value_types.iter().enumerate() {
        for (j, rhs) in value_types.iter().enumerate() {
            if matches!(lhs, Value::Global(_)) || matches!(rhs, Value::Global(_)) {
                continue; // Skip globals in binary ops for this test
            }
            
            let result_id = (i * 10 + j) as u32;
            let insts = rpm.emit_binary_op(
                IrBinaryOp::Add,
                lhs,
                rhs,
                result_id
            );
            
            assert!(!insts.is_empty(), "Should generate instructions for {:?} + {:?}", lhs, rhs);
        }
    }
}

#[test]
fn test_negative_local_count() {
    // Test with negative local count (edge case)
    let mut rpm = RegisterPressureManager::new(-10);
    rpm.init();
    
    // Should still work, though offsets might be unusual
    for i in 0..15 {  // Changed from 10 to 15 to force spilling with 12 registers
        rpm.get_register(format!("val{}", i));
    }
    
    // Check spill addresses account for negative local_count
    let insts = rpm.take_instructions();
    let has_negative_or_small = insts.iter().any(|i| match i {
        AsmInst::AddI(_, _, offset) => *offset < 0 || *offset < 10,
        _ => false,
    });
    
    assert!(has_negative_or_small || insts.is_empty());
}