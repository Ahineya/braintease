//! Centralized naming module for V2 backend
//! 
//! This module provides consistent, unique name generation for all temporary
//! values, registers, and labels used throughout the V2 backend. It ensures
//! no naming conflicts and maintains consistency between load/store operations.

use std::sync::atomic::{AtomicU32, Ordering};
use rcc_common::TempId;
use uuid::Uuid;

/// Global operation counter for unique IDs
/// Using atomic to ensure thread safety if needed in the future
static OPERATION_COUNTER: AtomicU32 = AtomicU32::new(0);

/// Name generator for V2 backend operations
/// 
/// This struct manages all name generation to ensure uniqueness and consistency.
/// Each instance represents a single compilation unit's naming context.
#[derive(Debug)]
pub struct NameGenerator {
    /// Operation ID for the current function being compiled
    function_id: u32,
    
    /// Counter for operations within the current function
    next_op_id: u32,
    
    /// Counter for unique labels within the current function
    next_label_id: u32,

    /// Per-instance tag so I32 labels do not collide across object files.
    unit_tag: u32,
}

impl NameGenerator {
    /// Create a new name generator for a function
    pub fn new(function_id: u32) -> Self {
        Self {
            function_id,
            next_op_id: 0,
            next_label_id: 0,
            unit_tag: Uuid::new_v4().as_u128() as u32,
        }
    }
    
    /// Get the next operation ID and increment counter
    pub fn next_operation_id(&mut self) -> u32 {
        let id = self.next_op_id;
        self.next_op_id += 1;
        id
    }
    
    /// Get the next label ID and increment counter
    pub fn next_label_id(&mut self) -> u32 {
        let id = self.next_label_id;
        self.next_label_id += 1;
        id
    }
    
    /// Get the function ID
    pub fn function_id(&self) -> u32 {
        self.function_id
    }
    
    // ===== Temp naming =====
    
    /// Get the standard name for a temp value
    pub fn temp_name(&self, temp_id: TempId) -> String {
        format!("t{temp_id}")
    }

    /// Companion high-word name for a 32-bit value whose low word is `lo_name`.
    /// Stable so later uses can find the high half after a load or binary op.
    pub fn i32_high_name(&self, lo_name: &str) -> String {
        format!("{lo_name}__hi")
    }

    pub fn i32_label(&mut self, kind: &str) -> String {
        let label_id = self.next_label_id();
        format!(
            "L_i32_{}_{:x}_{}_{}",
            kind, self.unit_tag, self.function_id, label_id
        )
    }
    
    // ===== Load operation naming =====
    
    /// Get name for a constant address loaded during a load operation
    pub fn load_const_addr(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("load_f{}_op{}_t{}_addr", self.function_id, op_id, result_temp)
    }
    
    /// Get name for the source pointer's bank tracking during a load operation
    /// This is used to track the bank info of the pointer being loaded FROM,
    /// not the result of the load
    pub fn load_src_ptr_bank(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("load_src_ptr_f{}_op{}_t{}", self.function_id, op_id, result_temp)
    }
    
    /// Get name for the bank address during pointer load
    pub fn load_bank_addr(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("load_f{}_op{}_t{}_bank_addr", self.function_id, op_id, result_temp)
    }
    
    /// Get name for the bank value register during pointer load
    pub fn load_bank_value(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("load_f{}_op{}_t{}_bank_val", self.function_id, op_id, result_temp)
    }
    
    /// Get name for a global address during load
    pub fn load_global_addr(&mut self, global_name: &str) -> String {
        let op_id = self.next_operation_id();
        format!("load_f{}_op{}_global_{}_addr", self.function_id, op_id, global_name)
    }
    
    /// Get context name for load operation bank runtime checking
    /// This generates a unique context string that includes the result temp
    pub fn load_bank_check_context(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("load_f{}_op{}_t{}", self.function_id, op_id, result_temp)
    }
    
    // ===== Store operation naming =====
    
    /// Get name for a constant value being stored
    pub fn store_const_value(&mut self) -> String {
        let op_id = self.next_operation_id();
        format!("store_f{}_op{}_const", self.function_id, op_id)
    }
    
    /// Get name for a fat pointer address during store
    pub fn store_fatptr_addr(&mut self) -> String {
        let op_id = self.next_operation_id();
        format!("store_f{}_op{}_fp_addr", self.function_id, op_id)
    }
    
    /// Get name for a fat pointer bank during store
    pub fn store_fatptr_bank(&mut self) -> String {
        let op_id = self.next_operation_id();
        format!("store_f{}_op{}_fp_bank", self.function_id, op_id)
    }
    
    /// Get name for destination address during store
    pub fn store_dest_addr(&mut self) -> String {
        let op_id = self.next_operation_id();
        format!("store_f{}_op{}_dest_addr", self.function_id, op_id)
    }
    
    /// Get name for bank store address
    pub fn store_bank_addr(&mut self) -> String {
        let op_id = self.next_operation_id();
        format!("store_f{}_op{}_bank_addr", self.function_id, op_id)
    }
    
    /// Get name for a global address during store
    pub fn store_global_addr(&mut self, global_name: &str) -> String {
        let op_id = self.next_operation_id();
        format!("store_f{}_op{}_global_{}_addr", self.function_id, op_id, global_name)
    }
    
    /// Get context name for store operation bank runtime checking
    /// This generates a unique context string for store operations
    pub fn store_bank_check_context(&mut self) -> String {
        let op_id = self.next_operation_id();
        format!("store_f{}_op{}", self.function_id, op_id)
    }
    
    // ===== Pointer bank tracking naming =====
    
    /// Get the key name for tracking a pointer's bank information
    /// This ensures consistency between load and store operations
    pub fn pointer_bank_key(&self, base_name: &str) -> String {
        // Use the base name directly for consistency
        // Don't add suffixes that might cause mismatches
        base_name.to_string()
    }
    
    /// Get the key name for a temp's pointer bank
    pub fn temp_pointer_bank_key(&self, temp_id: TempId) -> String {
        self.temp_name(temp_id)
    }
    
    // ===== Label naming =====
    
    /// Generate a label for loading a global
    pub fn load_global_label(&mut self, global_name: &str) -> String {
        let label_id = self.next_label_id();
        format!("load_global_{global_name}_{label_id}")
    }
    
    /// Generate a label for storing to a global
    pub fn store_global_label(&mut self, global_name: &str) -> String {
        let label_id = self.next_label_id();
        format!("store_global_{global_name}_{label_id}")
    }
    
    /// Generate a standard label name from a label ID
    /// This is used for branch targets and other control flow labels
    pub fn label_name(&self, label_id: rcc_common::LabelId) -> String {
        format!("L{label_id}")
    }
    
    /// Generate a label for a basic block within a function
    pub fn block_label(&self, function_name: &str, block_id: rcc_common::LabelId) -> String {
        format!("L_{function_name}_{block_id}")
    }
    
    /// Generate a label for the true branch of a select instruction
    pub fn select_true_label(&mut self, result_temp: TempId) -> String {
        let label_id = self.next_label_id();
        format!("L_select_true_f{}_l{}_t{}", self.function_id, label_id, result_temp)
    }
    
    /// Generate a label for the end of a select instruction
    pub fn select_end_label(&mut self, result_temp: TempId) -> String {
        let label_id = self.next_label_id();
        format!("L_select_end_f{}_l{}_t{}", self.function_id, label_id, result_temp)
    }
    
    // ===== GEP operation naming (future) =====
    
    /// Get name for GEP intermediate calculations
    pub fn gep_offset_temp(&mut self, base_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("gep_f{}_op{}_t{}_offset", self.function_id, op_id, base_temp)
    }
    
    /// Get name for GEP bank calculation
    pub fn gep_bank_temp(&mut self, base_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("gep_f{}_op{}_t{}_bank", self.function_id, op_id, base_temp)
    }
    
    // ===== Calling convention naming =====
    
    /// Get name for a function parameter
    pub fn param_name(&mut self, index: usize) -> String {
        let op_id = self.next_operation_id();
        format!("param_f{}_op{}_{}", self.function_id, op_id, index)
    }
    
    /// Get name for the bank part of a fat pointer parameter
    pub fn param_bank_name(&mut self, index: usize) -> String {
        let op_id = self.next_operation_id();
        format!("param_bank_f{}_op{}_{}", self.function_id, op_id, index)
    }
    
    /// Get name for return address register
    pub fn ret_addr_name(&mut self) -> String {
        let op_id = self.next_operation_id();
        format!("ret_addr_f{}_op{}", self.function_id, op_id)
    }
    
    /// Get name for return bank register
    pub fn ret_bank_name(&mut self) -> String {
        let op_id = self.next_operation_id();
        format!("ret_bank_f{}_op{}", self.function_id, op_id)
    }
    
    /// Get name for return value register
    pub fn ret_val_name(&mut self) -> String {
        let op_id = self.next_operation_id();
        format!("ret_val_f{}_op{}", self.function_id, op_id)
    }
    
    // ===== Function local naming =====
    
    /// Get name for a local variable at given offset
    pub fn local_name(&mut self, offset: i16) -> String {
        let op_id = self.next_operation_id();
        format!("local_f{}_op{}_off{}", self.function_id, op_id, offset)
    }
    
    /// Get name for a local variable address calculation
    pub fn local_addr_name(&mut self, offset: i16) -> String {
        let op_id = self.next_operation_id();
        format!("local_addr_f{}_op{}_off{}", self.function_id, op_id, offset)
    }
    
    // ===== Constant value naming =====
    
    /// Get name for a constant value register
    pub fn const_value(&mut self, value: i64) -> String {
        let op_id = self.next_operation_id();
        format!("const_f{}_op{}_{}", self.function_id, op_id, value)
    }
    
    /// Get name for a function address
    pub fn func_addr(&mut self, func_name: &str) -> String {
        let op_id = self.next_operation_id();
        format!("func_f{}_op{}_{}", self.function_id, op_id, func_name)
    }
    
    // ===== Unary operation naming =====
    
    /// Get name for all ones register (used in NOT operation)
    pub fn all_ones(&mut self) -> String {
        let op_id = self.next_operation_id();
        format!("all_ones_f{}_op{}", self.function_id, op_id)
    }
    
    /// Get name for zero register (used in NEG operation)
    pub fn zero_temp(&mut self) -> String {
        let op_id = self.next_operation_id();
        format!("zero_f{}_op{}", self.function_id, op_id)
    }
    
    /// Get name for mask register (used in truncation)
    pub fn mask_i8(&mut self) -> String {
        let op_id = self.next_operation_id();
        format!("mask_i8_f{}_op{}", self.function_id, op_id)
    }
    
    /// Get name for mask register (used in truncation to i1)
    pub fn mask_i1(&mut self) -> String {
        let op_id = self.next_operation_id();
        format!("mask_i1_f{}_op{}", self.function_id, op_id)
    }
    
    // ===== Comparison operation naming =====
    
    /// Get name for XOR temporary in comparison
    pub fn xor_temp(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("xor_temp_f{}_op{}_t{}", self.function_id, op_id, result_temp)
    }
    
    /// Get name for constant 1 in comparison
    pub fn const_one(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("const_1_f{}_op{}_t{}", self.function_id, op_id, result_temp)
    }
    
    /// Get name for constant 0 in comparison
    pub fn const_zero(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("const_0_f{}_op{}_t{}", self.function_id, op_id, result_temp)
    }
    
    /// Get name for SLE temporary
    pub fn sle_temp(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("sle_temp_f{}_op{}_t{}", self.function_id, op_id, result_temp)
    }
    
    /// Get name for SGE temporary
    pub fn sge_temp(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("sge_temp_f{}_op{}_t{}", self.function_id, op_id, result_temp)
    }
    
    /// Get name for ULE temporary
    pub fn ule_temp(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("ule_temp_f{}_op{}_t{}", self.function_id, op_id, result_temp)
    }
    
    /// Get name for UGE temporary
    pub fn uge_temp(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("uge_temp_f{}_op{}_t{}", self.function_id, op_id, result_temp)
    }
    
    // ===== Helper method naming for consolidated operations =====
    
    /// Get name for a constant associated with a temp
    /// Used when loading constants for specific operations
    pub fn const_for_temp(&mut self, temp_id: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("const_for_t{}_f{}_op{}", temp_id, self.function_id, op_id)
    }
    
    /// Get name for a temporary with context
    /// Used for generating contextual temporary names
    pub fn temp_with_context(&mut self, context: &str, suffix: &str) -> String {
        let op_id = self.next_operation_id();
        format!("{}_{}_{}_f{}_op{}", context, suffix, self.function_id, self.function_id, op_id)
    }
    
    // ===== Binary operation naming =====
    
    /// Get name for immediate value register
    pub fn imm_value(&mut self, value: i16) -> String {
        let op_id = self.next_operation_id();
        format!("imm_f{}_op{}_{}", self.function_id, op_id, value)
    }
    
    // ===== GEP additional naming =====
    
    /// Get name for GEP shift amount
    pub fn gep_shift(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("gep_shift_f{}_op{}_t{}", self.function_id, op_id, result_temp)
    }
    
    /// Get name for GEP element size
    pub fn gep_size(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("gep_size_f{}_op{}_t{}", self.function_id, op_id, result_temp)
    }
    
    /// Get name for GEP bank delta
    pub fn gep_bank_delta(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("gep_bank_delta_f{}_op{}_t{}", self.function_id, op_id, result_temp)
    }
    
    /// Get name for GEP bank size constant
    pub fn gep_bank_size(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("gep_bank_size_f{}_op{}_t{}", self.function_id, op_id, result_temp)
    }
    
    /// Get name for GEP new address
    pub fn gep_new_addr(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("gep_new_addr_f{}_op{}_t{}", self.function_id, op_id, result_temp)
    }
    
    /// Get name for GEP new bank
    pub fn gep_new_bank(&mut self, result_temp: TempId) -> String {
        let op_id = self.next_operation_id();
        format!("gep_new_bank_f{}_op{}_t{}", self.function_id, op_id, result_temp)
    }
    
    pub fn gep_stack_check_label(&mut self, result_temp: TempId) -> String {
        let label_id = self.next_label_id();
        format!("L_gep_stack_f{}_t{}_{}", self.function_id, result_temp, label_id)
    }
    
    pub fn gep_stack_end_label(&mut self, result_temp: TempId) -> String {
        // Use same counter as check label to keep them paired
        format!("L_gep_end_f{}_t{}_{}", self.function_id, result_temp, self.next_label_id - 1)
    }
    
    pub fn gep_reg_stack_check_label(&mut self, result_temp: TempId) -> String {
        let label_id = self.next_label_id();
        format!("L_gep_reg_stack_f{}_t{}_{}", self.function_id, result_temp, label_id)
    }
    
    pub fn gep_reg_stack_end_label(&mut self, result_temp: TempId) -> String {
        // Use same counter as check label to keep them paired
        format!("L_gep_reg_end_f{}_t{}_{}", self.function_id, result_temp, self.next_label_id - 1)
    }
    
    /// Get name for GEP global address
    pub fn gep_global(&mut self, global_name: &str) -> String {
        let op_id = self.next_operation_id();
        format!("gep_global_f{}_op{}_{}", self.function_id, op_id, global_name)
    }
    
    // ===== Bank check label naming =====
    
    /// Generate labels for bank check operations (use_global, use_stack, done)
    /// Returns a tuple of (use_global_label, use_stack_label, done_label)
    pub fn bank_check_labels(&mut self, _context: &str) -> (String, String, String) {
        // Use UUID for truly unique labels across all compilation units
        let unique_id = Uuid::new_v4().simple().to_string();
        let short_id = &unique_id[0..8]; // Use first 8 chars for shorter labels
        
        let use_global = format!("L_bc_{}_use_global", short_id);
        let use_stack = format!("L_bc_{}_use_stack", short_id);
        let done = format!("L_bc_{}_done", short_id);
        (use_global, use_stack, done)
    }
}

/// Global counter for translation units to ensure uniqueness across compilations
static TRANSLATION_UNIT_COUNTER: AtomicU32 = AtomicU32::new(0);

/// Create a new global name generator instance
/// Each function should get its own instance with a unique function ID
pub fn new_function_naming() -> NameGenerator {
    let function_id = OPERATION_COUNTER.fetch_add(1, Ordering::SeqCst);
    NameGenerator::new(function_id)
}

/// Get a unique translation unit ID for this compilation session
pub fn get_translation_unit_id() -> u32 {
    TRANSLATION_UNIT_COUNTER.fetch_add(1, Ordering::SeqCst)
}

#[cfg(test)]
mod tests {
    use super::*;
    
    #[test]
    fn test_unique_operation_ids() {
        let mut gen = NameGenerator::new(1);
        
        let id1 = gen.next_operation_id();
        let id2 = gen.next_operation_id();
        let id3 = gen.next_operation_id();
        
        assert_ne!(id1, id2);
        assert_ne!(id2, id3);
        assert_eq!(id1, 0);
        assert_eq!(id2, 1);
        assert_eq!(id3, 2);
    }
    
    #[test]
    fn test_load_naming_uniqueness() {
        let mut gen = NameGenerator::new(1);
        
        // Multiple loads to the same temp should get unique names
        let name1 = gen.load_const_addr(10);
        let name2 = gen.load_const_addr(10);
        
        assert_ne!(name1, name2);
        assert!(name1.contains("op0"));
        assert!(name2.contains("op1"));
    }
    
    #[test]
    fn test_store_naming_uniqueness() {
        let mut gen = NameGenerator::new(1);
        
        // Multiple stores should get unique names
        let name1 = gen.store_const_value();
        let name2 = gen.store_const_value();
        
        assert_ne!(name1, name2);
    }
    
    #[test]
    fn test_pointer_bank_key_consistency() {
        let gen = NameGenerator::new(1);
        
        // The key should be consistent for the same temp
        let key1 = gen.temp_pointer_bank_key(10);
        let key2 = gen.temp_pointer_bank_key(10);
        
        assert_eq!(key1, key2);
        assert_eq!(key1, "t10");
    }
    
    #[test]
    fn test_function_isolation() {
        let mut gen1 = NameGenerator::new(1);
        let mut gen2 = NameGenerator::new(2);
        
        let name1 = gen1.load_const_addr(10);
        let name2 = gen2.load_const_addr(10);
        
        // Same operation in different functions should have different names
        assert_ne!(name1, name2);
        assert!(name1.contains("f1"));
        assert!(name2.contains("f2"));
    }
}