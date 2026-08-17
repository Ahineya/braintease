//! Integration tests for GEP with other instructions
//! 
//! Tests that GEP works correctly with load/store operations
//! and within the context of functions.

use rcc_frontend::ir::{Value, IrType, FatPointer};
use crate::instr::{lower_gep, lower_store, lower_load};
use crate::regmgmt::{RegisterPressureManager, BankInfo};
use crate::naming::new_function_naming;
use rcc_codegen::{AsmInst, Reg};
use rcc_common::TempId;
use rcc_frontend::BankTag;

const BANK_SIZE_INSTRUCTIONS: u16 = 4096;

#[test]
fn test_gep_then_store() {
    // Test: arr[5] = 42
    // This tests GEP followed by store to the computed address
    
    let mut mgr = RegisterPressureManager::new(10); // Some locals
    mgr.init();
    let mut naming = new_function_naming();
    
    // Create an array base pointer (e.g., from alloca)
    let array_base = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Temp(1)),
        bank: BankTag::Stack,
    });
    
    // Set up the base pointer in register manager
    mgr.set_pointer_bank("t1".to_string(), BankInfo::Stack);
    
    // Step 1: GEP to calculate &arr[5]
    let index = Value::Constant(5);
    let element_size = 1; // 16-bit integers = 1 cell each
    let gep_result_temp: TempId = 10;
    
    let mut insts = lower_gep(&mut mgr, &mut naming, &array_base, &[index], element_size, gep_result_temp, BANK_SIZE_INSTRUCTIONS);
    
    // Step 2: Store value 42 to the computed address
    let value_to_store = Value::Constant(42);
    let ptr_to_store = Value::Temp(gep_result_temp);
    
    let store_insts = lower_store(&mut mgr, &mut naming, &value_to_store, &ptr_to_store, &IrType::I16);
    insts.extend(store_insts);
    
    // Verify we have both GEP and store operations
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::AddI(_, _, 5))), 
            "Should calculate offset for index 5");
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Store(_, _, _))), 
            "Should generate store instruction");
    
    // Verify the store uses the correct bank (should be Stack/SB)
    let store_with_sb = insts.iter().any(|inst| {
        if let AsmInst::Store(_, bank, _) = inst {
            *bank == Reg::Sb
        } else {
            false
        }
    });
    assert!(store_with_sb, "Store should use SB for stack-based array");
}

#[test]
fn test_gep_then_load() {
    // Test: x = arr[3]
    // This tests GEP followed by load from the computed address
    
    let mut mgr = RegisterPressureManager::new(5);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Create an array base pointer
    let array_base = Value::Temp(20);
    mgr.set_pointer_bank("t20".to_string(), BankInfo::Global);
    
    // Step 1: GEP to calculate &arr[3]
    let index = Value::Constant(3);
    let element_size = 1;
    let gep_result_temp: TempId = 30;
    
    let mut insts = lower_gep(&mut mgr, &mut naming, &array_base, &[index], element_size, gep_result_temp, BANK_SIZE_INSTRUCTIONS);
    
    // Step 2: Load from the computed address
    let ptr_to_load = Value::Temp(gep_result_temp);
    let load_result_temp: TempId = 40;
    let result_type = IrType::I16; // Loading a 16-bit integer
    
    let load_insts = lower_load(&mut mgr, &mut naming, &ptr_to_load, &result_type, load_result_temp);
    insts.extend(load_insts);
    
    // Verify we have both GEP and load operations
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::AddI(_, _, 3))), 
            "Should calculate offset for index 3");
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Load(_, _, _))), 
            "Should generate load instruction");
    
    // Verify the load uses the correct bank (should be Global/GP)
    let load_with_gp = insts.iter().any(|inst| {
        if let AsmInst::Load(_, bank, _) = inst {
            *bank == Reg::Gp
        } else {
            false
        }
    });
    assert!(load_with_gp, "Load should use GP for global array");
}

#[test]
fn test_gep_with_dynamic_index_then_store() {
    // Test: arr[i] = value
    // This tests GEP with dynamic index followed by store
    
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Array base and dynamic index
    let array_base = Value::Temp(50);
    mgr.set_pointer_bank("t50".to_string(), BankInfo::Stack);
    
    let index = Value::Temp(51); // Dynamic index in register
    let element_size = 2; // Fat pointers = 2 cells each
    let gep_result_temp: TempId = 60;
    
    let mut insts = lower_gep(&mut mgr, &mut naming, &array_base, &[index], element_size, gep_result_temp, BANK_SIZE_INSTRUCTIONS);
    
    // Store a value to the computed address
    let value_to_store = Value::Temp(52);
    let ptr_to_store = Value::Temp(gep_result_temp);
    
    let store_insts = lower_store(&mut mgr, &mut naming, &value_to_store, &ptr_to_store, &IrType::I16);
    insts.extend(store_insts);
    
    // Should have runtime bank overflow handling
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Div(_, _, _))), 
            "Should calculate bank delta for dynamic index");
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Mod(_, _, _))), 
            "Should calculate address within bank");
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Store(_, _, _))), 
            "Should generate store instruction");
}

#[test]
fn test_gep_chain() {
    // Test: arr[i][j] - chained GEP operations
    // This simulates accessing a 2D array
    
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Base pointer for 2D array
    let array_base = Value::Temp(100);
    mgr.set_pointer_bank("t100".to_string(), BankInfo::Stack);
    
    // First GEP: calculate &arr[i] where each row has 10 elements
    let i_index = Value::Constant(3);
    let row_size = 10; // 10 elements per row
    let first_gep_temp: TempId = 110;
    
    let mut insts = lower_gep(&mut mgr, &mut naming, &array_base, &[i_index], row_size, first_gep_temp, BANK_SIZE_INSTRUCTIONS);
    
    // Second GEP: calculate &arr[i][j]
    let j_index = Value::Constant(7);
    let element_size = 1;
    let second_gep_temp: TempId = 120;
    
    let first_result = Value::Temp(first_gep_temp);
    let second_gep_insts = lower_gep(&mut mgr, &mut naming, &first_result, &[j_index], element_size, second_gep_temp, BANK_SIZE_INSTRUCTIONS);
    insts.extend(second_gep_insts);
    
    // Should calculate offset = 3*10 + 7 = 37
    // First GEP adds 30, second adds 7
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::AddI(_, _, 30))), 
            "First GEP should add 3*10=30");
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::AddI(_, _, 7))), 
            "Second GEP should add 7");
    
    // Result should maintain stack bank
    assert!(matches!(
        mgr.get_pointer_bank(&format!("t{}", second_gep_temp)),
        Some(BankInfo::Stack)
    ), "Final pointer should maintain stack bank");
}

#[test]
fn test_gep_in_loop_pattern() {
    // Test a common loop pattern: for(i=0; i<n; i++) arr[i] = i;
    // This tests multiple GEP operations with increasing indices
    
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    let array_base = Value::Temp(200);
    mgr.set_pointer_bank("t200".to_string(), BankInfo::Stack);
    
    let mut all_insts = vec![];
    
    // Simulate loop iterations
    for i in 0..3 {
        let index = Value::Constant(i);
        let element_size = 1;
        let gep_temp = 210 + i as TempId;
        
        let gep_insts = lower_gep(&mut mgr, &mut naming, &array_base, &[index], element_size, gep_temp, BANK_SIZE_INSTRUCTIONS);
        all_insts.extend(gep_insts);
        
        // Store i to arr[i]
        let value = Value::Constant(i);
        let ptr = Value::Temp(gep_temp);
        let store_insts = lower_store(&mut mgr, &mut naming, &value, &ptr, &IrType::I16);
        all_insts.extend(store_insts);
    }
    
    // Should have GEP operations for indices 1 and 2 (index 0 just copies base)
    // Index 0: copies base (Add with R0)
    // Index 1: AddI(_, _, 1)
    // Index 2: AddI(_, _, 2)
    assert!(all_insts.iter().any(|inst| matches!(inst, AsmInst::Add(_, _, r) if *r == Reg::R0)),
            "Index 0 should copy base address");
    assert!(all_insts.iter().any(|inst| matches!(inst, AsmInst::AddI(_, _, 1))),
            "Should have offset 1");
    assert!(all_insts.iter().any(|inst| matches!(inst, AsmInst::AddI(_, _, 2))),
            "Should have offset 2");
    
    let store_count = all_insts.iter().filter(|inst| {
        matches!(inst, AsmInst::Store(_, _, _))
    }).count();
    assert_eq!(store_count, 3, "Should have 3 store operations");
}

#[test]
fn test_gep_with_large_array_crossing_banks() {
    // Test accessing an element in a large array that crosses bank boundaries
    // With BANK_SIZE = 4096, accessing element 5000 should trigger bank overflow
    
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Large array base
    let array_base = Value::Temp(300);
    mgr.set_pointer_bank("t300".to_string(), BankInfo::Stack);
    
    // Access element 5000 (will cross bank boundary)
    let index = Value::Constant(5000);
    let element_size = 1;
    let gep_temp: TempId = 310;
    
    let mut insts = lower_gep(&mut mgr, &mut naming, &array_base, &[index], element_size, gep_temp, BANK_SIZE_INSTRUCTIONS);
    
    // Load from the computed address
    let ptr = Value::Temp(gep_temp);
    let load_temp: TempId = 320;
    let load_insts = lower_load(&mut mgr, &mut naming, &ptr, &IrType::I16, load_temp);
    insts.extend(load_insts);
    
    // Should have bank overflow warning
    assert!(insts.iter().any(|inst| {
        if let AsmInst::Comment(s) = inst {
            s.contains("bank overflow")
        } else {
            false
        }
    }), "Should warn about bank overflow for large offset");
    
    // Should still generate valid load
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Load(_, _, _))), 
            "Should generate load despite bank crossing");
}

#[test]
fn test_gep_load_store_pointer() {
    // Test loading/storing fat pointers through GEP
    // arr[2] = &some_value; ptr = arr[2];
    
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Array of pointers
    let array_base = Value::Temp(400);
    mgr.set_pointer_bank("t400".to_string(), BankInfo::Stack);
    
    // GEP to calculate &arr[2]
    let index = Value::Constant(2);
    let element_size = 2; // Fat pointers are 2 cells
    let gep_temp: TempId = 410;
    
    let mut insts = lower_gep(&mut mgr, &mut naming, &array_base, &[index], element_size, gep_temp, BANK_SIZE_INSTRUCTIONS);
    
    // Store a fat pointer to arr[2]
    let ptr_value = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Constant(0x1000)),
        bank: BankTag::Global,
    });
    let store_ptr = Value::Temp(gep_temp);
    
    let store_insts = lower_store(&mut mgr, &mut naming, &ptr_value, &store_ptr, &IrType::FatPtr(Box::new(IrType::I16)));
    insts.extend(store_insts);
    
    // Should store both components of fat pointer
    let store_count = insts.iter().filter(|inst| {
        matches!(inst, AsmInst::Store(_, _, _))
    }).count();
    assert!(store_count >= 2, "Should store both address and bank for fat pointer");
    
    // Now load the fat pointer back
    let load_temp: TempId = 420;
    let ptr_type = IrType::FatPtr(Box::new(IrType::I16));
    let load_insts = lower_load(&mut mgr, &mut naming, &store_ptr, &ptr_type, load_temp);
    insts.extend(load_insts);
    
    // Should load both components
    let load_count = insts.iter().filter(|inst| {
        matches!(inst, AsmInst::Load(_, _, _))
    }).count();
    assert!(load_count >= 2, "Should load both address and bank for fat pointer");
}

#[test]
fn test_gep_chain_with_stores_and_loads() {
    // Complex test: Simulate matrix operations with chained GEPs, stores, and loads
    // matrix[i][j] = value; then x = matrix[i][j];
    
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    // 2D matrix base pointer (e.g., int matrix[10][10])
    let matrix_base = Value::Temp(500);
    mgr.set_pointer_bank("t500".to_string(), BankInfo::Stack);
    
    let mut all_insts = vec![];
    
    // Step 1: Store values to matrix[2][3] and matrix[2][4]
    for j in 3..5 {
        // First GEP: calculate &matrix[2] (row 2, each row = 10 elements)
        let row_index = Value::Constant(2);
        let row_size = 10;
        let row_gep_temp = 510 + (j * 10) as TempId;
        
        let row_gep_insts = lower_gep(&mut mgr, &mut naming, &matrix_base, 
                                      &[row_index], row_size, row_gep_temp, BANK_SIZE_INSTRUCTIONS);
        all_insts.extend(row_gep_insts);
        
        // Second GEP: calculate &matrix[2][j]
        let col_index = Value::Constant(j);
        let element_size = 1;
        let elem_gep_temp = row_gep_temp + 1;
        
        let row_ptr = Value::Temp(row_gep_temp);
        let elem_gep_insts = lower_gep(&mut mgr, &mut naming, &row_ptr, 
                                       &[col_index], element_size, elem_gep_temp, BANK_SIZE_INSTRUCTIONS);
        all_insts.extend(elem_gep_insts);
        
        // Store value to matrix[2][j]
        let value = Value::Constant(20 + j); // Store 23 and 24
        let elem_ptr = Value::Temp(elem_gep_temp);
        let store_insts = lower_store(&mut mgr, &mut naming, &value, &elem_ptr, &IrType::I16);
        all_insts.extend(store_insts);
    }
    
    // Step 2: Load values from matrix[2][3] and matrix[2][4]
    for j in 3..5 {
        // First GEP: calculate &matrix[2]
        let row_index = Value::Constant(2);
        let row_size = 10;
        let row_gep_temp = 600 + (j * 10) as TempId;
        
        let row_gep_insts = lower_gep(&mut mgr, &mut naming, &matrix_base, 
                                      &[row_index], row_size, row_gep_temp, BANK_SIZE_INSTRUCTIONS);
        all_insts.extend(row_gep_insts);
        
        // Second GEP: calculate &matrix[2][j]
        let col_index = Value::Constant(j);
        let element_size = 1;
        let elem_gep_temp = row_gep_temp + 1;
        
        let row_ptr = Value::Temp(row_gep_temp);
        let elem_gep_insts = lower_gep(&mut mgr, &mut naming, &row_ptr, 
                                       &[col_index], element_size, elem_gep_temp, BANK_SIZE_INSTRUCTIONS);
        all_insts.extend(elem_gep_insts);
        
        // Load from matrix[2][j]
        let elem_ptr = Value::Temp(elem_gep_temp);
        let load_temp = elem_gep_temp + 2;
        let load_insts = lower_load(&mut mgr, &mut naming, &elem_ptr, 
                                    &IrType::I16, load_temp);
        all_insts.extend(load_insts);
    }
    
    // Verify we have the expected operations
    // 4 first-level GEPs (2 for stores, 2 for loads) calculating row offset
    let row_gep_count = all_insts.iter().filter(|inst| {
        matches!(inst, AsmInst::AddI(_, _, 20)) // row 2 * 10 = 20
    }).count();
    assert_eq!(row_gep_count, 4, "Should have 4 row GEP calculations");
    
    // 4 second-level GEPs calculating column offsets
    let col3_count = all_insts.iter().filter(|inst| {
        matches!(inst, AsmInst::AddI(_, _, 3))
    }).count();
    let col4_count = all_insts.iter().filter(|inst| {
        matches!(inst, AsmInst::AddI(_, _, 4))
    }).count();
    assert!(col3_count >= 2, "Should calculate column 3 offset at least twice");
    assert!(col4_count >= 2, "Should calculate column 4 offset at least twice");
    
    // 2 stores and 2 loads
    let store_count = all_insts.iter().filter(|inst| {
        matches!(inst, AsmInst::Store(_, _, _))
    }).count();
    let load_count = all_insts.iter().filter(|inst| {
        matches!(inst, AsmInst::Load(_, _, _))
    }).count();
    assert_eq!(store_count, 2, "Should have 2 store operations");
    assert_eq!(load_count, 2, "Should have 2 load operations");
}

#[test]
fn test_gep_with_dynamic_2d_access() {
    // Test: matrix[i][j] = value where both i and j are dynamic
    // This tests the most complex case with runtime bank overflow handling
    
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Dynamic 2D access
    let matrix_base = Value::Temp(700);
    mgr.set_pointer_bank("t700".to_string(), BankInfo::Stack);
    
    // Dynamic indices
    let i_index = Value::Temp(701); // Dynamic row index
    let j_index = Value::Temp(702); // Dynamic column index
    
    // First GEP: calculate &matrix[i] with dynamic row
    let row_size = 100; // Large row to potentially cross banks
    let row_gep_temp: TempId = 710;
    
    let mut insts = lower_gep(&mut mgr, &mut naming, &matrix_base, 
                              &[i_index], row_size, row_gep_temp, BANK_SIZE_INSTRUCTIONS);
    
    // Should have runtime bank calculation for large row size
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Mul(_, _, _))),
            "Should multiply i by row_size");
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Div(_, _, _))),
            "Should calculate bank delta for dynamic row");
    assert!(insts.iter().any(|inst| matches!(inst, AsmInst::Mod(_, _, _))),
            "Should calculate address within bank");
    
    // Second GEP: calculate &matrix[i][j]
    let element_size = 1;
    let elem_gep_temp: TempId = 720;
    
    let row_ptr = Value::Temp(row_gep_temp);
    let elem_gep_insts = lower_gep(&mut mgr, &mut naming, &row_ptr, 
                                   &[j_index], element_size, elem_gep_temp, BANK_SIZE_INSTRUCTIONS);
    insts.extend(elem_gep_insts);
    
    // The second GEP might also need bank calculations if the first resulted in dynamic bank
    // Result should have dynamic bank
    assert!(matches!(
        mgr.get_pointer_bank(&format!("t{}", elem_gep_temp)),
        Some(BankInfo::Dynamic(_))
    ), "Result should have dynamic bank after two dynamic GEPs");
    
    // Store a value
    let value = Value::Constant(999);
    let elem_ptr = Value::Temp(elem_gep_temp);
    let store_insts = lower_store(&mut mgr, &mut naming, &value, &elem_ptr, &IrType::I16);
    insts.extend(store_insts);
    
    // Store should use dynamic bank register
    let has_dynamic_store = insts.iter().any(|inst| {
        if let AsmInst::Store(_, bank, _) = inst {
            // Check if it's not one of the static banks
            *bank != Reg::Sb && *bank != Reg::Gp
        } else {
            false
        }
    });
    assert!(has_dynamic_store, "Store should use dynamic bank register");
}

#[test]
fn test_gep_struct_field_simulation() {
    // Simulate accessing struct fields through GEP
    // struct Point { int x; int y; int* next; }
    // point.y = 10; point.next = &other_point;
    
    let mut mgr = RegisterPressureManager::new(0);
    mgr.init();
    let mut naming = new_function_naming();
    
    // Struct base pointer
    let struct_base = Value::Temp(800);
    mgr.set_pointer_bank("t800".to_string(), BankInfo::Stack);
    
    let mut all_insts = vec![];
    
    // Access field y (offset 1)
    let y_offset = Value::Constant(1);
    let y_gep_temp: TempId = 810;
    let y_gep_insts = lower_gep(&mut mgr, &mut naming, &struct_base, 
                                &[y_offset], 1, y_gep_temp, BANK_SIZE_INSTRUCTIONS);
    all_insts.extend(y_gep_insts);
    
    // Store to y field
    let y_value = Value::Constant(10);
    let y_ptr = Value::Temp(y_gep_temp);
    let y_store_insts = lower_store(&mut mgr, &mut naming, &y_value, &y_ptr, &IrType::I16);
    all_insts.extend(y_store_insts);
    
    // Access field next (offset 2, fat pointer field)
    let next_offset = Value::Constant(2);
    let next_gep_temp: TempId = 820;
    let next_gep_insts = lower_gep(&mut mgr, &mut naming, &struct_base, 
                                   &[next_offset], 1, next_gep_temp, BANK_SIZE_INSTRUCTIONS);
    all_insts.extend(next_gep_insts);
    
    // Store a fat pointer to next field
    let next_value = Value::FatPtr(FatPointer {
        addr: Box::new(Value::Constant(0x2000)),
        bank: BankTag::Global,
    });
    let next_ptr = Value::Temp(next_gep_temp);
    let next_store_insts = lower_store(&mut mgr, &mut naming, &next_value, &next_ptr, &IrType::FatPtr(Box::new(IrType::I16)));
    all_insts.extend(next_store_insts);
    
    // Load the pointer back
    let load_temp: TempId = 830;
    let ptr_type = IrType::FatPtr(Box::new(IrType::I16));
    let load_insts = lower_load(&mut mgr, &mut naming, &next_ptr, &ptr_type, load_temp);
    all_insts.extend(load_insts);
    
    // Verify field accesses
    assert!(all_insts.iter().any(|inst| matches!(inst, AsmInst::AddI(_, _, 1))),
            "Should calculate offset for y field");
    assert!(all_insts.iter().any(|inst| matches!(inst, AsmInst::AddI(_, _, 2))),
            "Should calculate offset for next field");
    
    // Should have stores for both scalar and fat pointer
    let store_count = all_insts.iter().filter(|inst| {
        matches!(inst, AsmInst::Store(_, _, _))
    }).count();
    assert!(store_count >= 3, "Should store y (1) and next pointer (2 components)");
    
    // Should load both components of fat pointer
    let load_count = all_insts.iter().filter(|inst| {
        matches!(inst, AsmInst::Load(_, _, _))
    }).count();
    assert!(load_count >= 2, "Should load both components of fat pointer");
}