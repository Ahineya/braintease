//! High-level, safe API for building functions
//! 
//! This module provides a builder pattern that ensures correct function generation
//! without exposing internal complexity or allowing misuse.
//! 
//! ## Design Philosophy
//! 
//! This API follows the principle of "make illegal states unrepresentable":
//! - You cannot emit epilogue before prologue
//! - You cannot forget to clean up the stack after calls
//! - You cannot access locals before the prologue
//! - You cannot have mismatched call/cleanup sequences
//! - You cannot access internal state that could break invariants
//! 
//! If you need additional functionality, add a proper safe method rather than
//! exposing internals. The goal is an API that is impossible to misuse.

use crate::regmgmt::{BankInfo, RegisterPressureManager};
use rcc_codegen::{AsmInst, Reg};
use super::internal::FunctionLowering;
use super::calling_convention::CallingConvention;
use log::{debug, trace, info};
// Re-export types needed for the public API
pub use super::calling_convention::{CallArg, CallTarget};

/// High-level function builder that encapsulates all the complexity
/// 
/// This builder ensures:
/// - Correct ordering of operations (prologue before epilogue)
/// - Automatic stack tracking
/// - Proper naming context management
/// - No manual instruction vector management
pub struct FunctionBuilder {
    /// Internal function lowering state
    func: FunctionLowering,
    
    /// Calling convention helper (stateless, so we can just own one)
    cc: CallingConvention,
    
    /// Accumulated instructions
    instructions: Vec<AsmInst>,
    
    /// Track if prologue has been emitted
    prologue_emitted: bool,
    
    /// Track if epilogue has been emitted
    epilogue_emitted: bool,
    
    /// Stack of pending cleanups (for nested calls)
    cleanup_stack: Vec<i16>,
    
    /// Parameter types for this function (needed for correct stack offset calculation)
    param_types: Vec<(rcc_common::TempId, rcc_frontend::ir::IrType)>,
}

impl Default for FunctionBuilder {
    fn default() -> Self {
        Self::new()
    }
}

impl FunctionBuilder {
    /// Make a standalone function call without a FunctionBuilder context
    /// 
    /// This is used when lowering individual Call instructions outside of function building
    pub fn make_standalone_call(
        mgr: &mut RegisterPressureManager,
        naming: &mut crate::naming::NameGenerator,
        target: super::calling_convention::CallTarget,
        args: Vec<CallArg>,
        returns_pointer: bool,
        result_name: Option<String>,
        stack_args_from: Option<usize>,
    ) -> Vec<AsmInst> {
        let cc = CallingConvention::new();
        let (call_insts, _return_regs) = cc.make_complete_call(
            mgr,
            naming,
            target,
            args,
            returns_pointer,
            result_name,
            stack_args_from,
        );
        call_insts
    }

    /// FP-relative offset of the first unnamed argument in a variadic callee.
    pub fn first_vararg_fp_offset(
        param_types: &[(rcc_common::TempId, rcc_frontend::ir::IrType)],
    ) -> i16 {
        CallingConvention::new().first_vararg_offset(param_types)
    }
    
    /// Create a new function builder
    pub fn new() -> Self {
        info!("Creating new FunctionBuilder");
        Self {
            func: FunctionLowering::new(),
            cc: CallingConvention::new(),
            instructions: Vec::new(),
            prologue_emitted: false,
            epilogue_emitted: false,
            cleanup_stack: Vec::new(),
            param_types: Vec::new(),
        }
    }
    
    /// Create a new function builder with parameter types
    /// This should be used when lowering IR functions that have parameter type information
    pub fn with_params(param_types: Vec<(rcc_common::TempId, rcc_frontend::ir::IrType)>) -> Self {
        info!("Creating new FunctionBuilder with {} parameters", param_types.len());
        Self {
            func: FunctionLowering::new(),
            cc: CallingConvention::new(),
            instructions: Vec::new(),
            prologue_emitted: false,
            epilogue_emitted: false,
            cleanup_stack: Vec::new(),
            param_types,
        }
    }
    
    /// Start the function with a prologue
    /// 
    /// Must be called before any other operations
    pub fn begin_function(&mut self, local_slots: i16) -> &mut Self {
        debug!("Beginning function with {local_slots} local slots");
        assert!(!self.prologue_emitted, "Prologue already emitted");
        
        let prologue = self.func.emit_prologue(local_slots);
        trace!("  Prologue generated {} instructions", prologue.len());
        self.instructions.extend(prologue);
        self.prologue_emitted = true;
        self
    }
    
    /// Load a parameter into a register with full register management
    /// 
    /// This version takes the register manager and naming context to properly bind the parameter
    /// Returns (address_register, optional_bank_register)
    pub fn load_parameter_with_binding(
        &mut self,
        index: usize,
        param_id: rcc_common::TempId,
        mgr: &mut RegisterPressureManager,
        naming: &mut crate::naming::NameGenerator
    ) -> (Reg, Option<Reg>) {
        debug!("Loading and binding parameter {index} (t{param_id})");
        assert!(self.prologue_emitted, "Must emit prologue before loading parameters");
        assert!(!self.epilogue_emitted, "Cannot load parameters after epilogue");
        
        // Use the calling convention to load the parameter
        let (param_insts, addr_reg, bank_reg) = self.cc.load_param(index, &self.param_types, mgr, naming);
        self.instructions.extend(param_insts);
        
        // Bind the parameter to the register manager
        let param_name = naming.temp_name(param_id);
        mgr.bind_value_to_register(param_name.clone(), addr_reg);
        
        // If this is a fat pointer, track the bank as a named value so it
        // survives register reuse of A1/A3.
        if let Some(bank) = bank_reg {
            debug!("  Parameter {index} is a fat pointer with bank in {bank:?}");
            let bank_name = naming.param_bank_name(index);
            mgr.bind_value_to_register(bank_name.clone(), bank);
            mgr.set_pointer_bank(param_name, BankInfo::Dynamic(bank_name));
        }
        
        trace!("  Parameter {index} bound to {addr_reg:?} (bank: {bank_reg:?})");
        (addr_reg, bank_reg)
    }
    
    /// Load a parameter into a register (simple version without binding)
    /// 
    /// Returns the register containing the parameter
    pub fn load_parameter(&mut self, index: usize) -> Reg {
        debug!("Loading parameter {index} (simple)");
        assert!(self.prologue_emitted, "Must emit prologue before loading parameters");
        assert!(!self.epilogue_emitted, "Cannot load parameters after epilogue");
        
        let (insts, reg, bank_reg) = self.func.load_param(index, &self.param_types);
        trace!("  Parameter {} loaded into {:?} (bank in {:?}), {} instructions generated", index, reg, bank_reg, insts.len());
        self.instructions.extend(insts);
        // Note: For now, we're only returning the address register
        // The caller should use the register manager to track the bank if needed
        reg
    }
    
    /// Get the address of a local variable
    pub fn get_local_address(&mut self, offset: i16) -> Reg {
        debug!("Getting address of local at offset {offset}");
        assert!(self.prologue_emitted, "Must emit prologue before accessing locals");
        assert!(!self.epilogue_emitted, "Cannot access locals after epilogue");
        
        let reg = self.func.get_local_addr(offset);
        let func_insts = self.func.instructions.drain(..).collect::<Vec<_>>();
        trace!("  Local address in {:?}, {} instructions generated", reg, func_insts.len());
        self.instructions.extend(func_insts);
        reg
    }
    
    /// Load from a local variable
    pub fn load_local(&mut self, offset: i16, dest: Reg) -> &mut Self {
        assert!(self.prologue_emitted, "Must emit prologue before accessing locals");
        assert!(!self.epilogue_emitted, "Cannot access locals after epilogue");
        
        let insts = self.func.load_local(offset, dest);
        self.instructions.extend(insts);
        self
    }
    
    /// Store to a local variable
    pub fn store_local(&mut self, offset: i16, src: Reg) -> &mut Self {
        assert!(self.prologue_emitted, "Must emit prologue before accessing locals");
        assert!(!self.epilogue_emitted, "Cannot access locals after epilogue");
        
        let insts = self.func.store_local(offset, src);
        self.instructions.extend(insts);
        self
    }
    
    /// Make a function call with automatic stack management
    /// 
    /// This version requires register manager and naming for proper call handling
    /// Uses the unified make_complete_call interface
    pub fn call_function_complete(
        &mut self,
        func_addr: u16,
        func_bank: u16,
        args: Vec<CallArg>,
        returns_pointer: bool,
        result_name: Option<String>,
        mgr: &mut RegisterPressureManager,
        naming: &mut crate::naming::NameGenerator
    ) -> (Reg, Option<Reg>) {
        info!("Calling function at bank:{}, addr:{} with {} args", 
              func_bank, func_addr, args.len());
        assert!(self.prologue_emitted, "Must emit prologue before making calls");
        assert!(!self.epilogue_emitted, "Cannot make calls after epilogue");
        
        // Use the unified calling convention interface
        let (call_insts, return_regs) = self.cc.make_complete_call(
            mgr,
            naming,
            super::calling_convention::CallTarget::Address { addr: func_addr, bank: func_bank },
            args,
            returns_pointer,
            result_name,
            None,
        );
        
        self.instructions.extend(call_insts);
        
        // Extract the return registers
        let (ret_addr, ret_bank) = return_regs.unwrap_or((Reg::Rv0, None));
        debug!("Call complete: return in {ret_addr:?}, bank: {ret_bank:?}");
        (ret_addr, ret_bank)
    }
    
    /// Make a function call (simple version for tests)
    /// 
    /// This method:
    /// - Sets up arguments on the stack
    /// - Emits the call instruction
    /// - Returns registers containing the return value
    /// - Automatically tracks stack cleanup needed
    pub fn call_function(
        &mut self, 
        func_addr: u16, 
        func_bank: u16,
        args: Vec<CallArg>,
        returns_pointer: bool
    ) -> (Reg, Option<Reg>) {
        info!("Calling function at bank:{}, addr:{} with {} args (simple)", 
              func_bank, func_addr, args.len());
        assert!(self.prologue_emitted, "Must emit prologue before making calls");
        assert!(!self.epilogue_emitted, "Cannot make calls after epilogue");
        
        // Calculate stack words needed
        let stack_words = args.iter().map(|arg| match arg {
            CallArg::Scalar(_) => 1,
            CallArg::FatPointer { .. } => 2,
        }).sum::<i16>();
        debug!("  Call will use {stack_words} stack words");
        
        // Setup arguments
        trace!("  Setting up call arguments");
        let setup = self.func.setup_call(args);
        self.instructions.extend(setup);
        
        // Emit call
        trace!("  Emitting call instruction");
        let call = self.cc.emit_call(func_addr, func_bank);
        self.instructions.extend(call);
        
        // Handle return value
        debug!("  Handling return value (is_pointer: {returns_pointer})");
        let (ret_insts, (ret_addr, ret_bank)) = self.func.handle_call_return(returns_pointer);
        self.instructions.extend(ret_insts);
        
        // Cleanup stack
        trace!("  Cleaning up {stack_words} stack words");
        let cleanup = self.cc.cleanup_stack(stack_words);
        self.instructions.extend(cleanup);
        
        debug!("Call complete: return in {ret_addr:?}, bank: {ret_bank:?}");
        (ret_addr, ret_bank)
    }
    
    /// Begin a nested call sequence
    /// 
    /// Use this when you need to manually manage parts of a call
    pub fn begin_call(&mut self, args: Vec<CallArg>) -> &mut Self {
        debug!("Beginning manual call sequence with {} args", args.len());
        assert!(self.prologue_emitted, "Must emit prologue before making calls");
        assert!(!self.epilogue_emitted, "Cannot make calls after epilogue");
        
        // Calculate and track cleanup needed
        let stack_words = args.iter().map(|arg| match arg {
            CallArg::Scalar(_) => 1,
            CallArg::FatPointer { .. } => 2,
        }).sum::<i16>();
        
        trace!("  Pushing {stack_words} words to cleanup stack");
        self.cleanup_stack.push(stack_words);
        
        // Setup arguments
        let setup = self.func.setup_call(args);
        trace!("  Setup generated {} instructions", setup.len());
        self.instructions.extend(setup);
        self
    }
    
    /// Emit the actual call instruction
    pub fn emit_call(&mut self, func_addr: u16, func_bank: u16) -> &mut Self {
        let call = self.cc.emit_call(func_addr, func_bank);
        self.instructions.extend(call);
        self
    }
    
    /// Handle return value from the last call
    pub fn get_return_value(&mut self, is_pointer: bool) -> (Reg, Option<Reg>) {
        let (ret_insts, regs) = self.func.handle_call_return(is_pointer);
        self.instructions.extend(ret_insts);
        regs
    }
    
    /// End a call sequence (performs stack cleanup)
    pub fn end_call(&mut self) -> &mut Self {
        let stack_words = self.cleanup_stack.pop()
            .expect("end_call called without matching begin_call");
        
        debug!("Ending call sequence, cleaning up {stack_words} stack words");
        let cleanup = self.cc.cleanup_stack(stack_words);
        trace!("  Cleanup generated {} instructions", cleanup.len());
        self.instructions.extend(cleanup);
        self
    }
    
    /// Add a custom instruction
    pub fn add_instruction(&mut self, inst: AsmInst) -> &mut Self {
        assert!(self.prologue_emitted, "Must emit prologue first");
        assert!(!self.epilogue_emitted, "Cannot add instructions after epilogue");
        
        self.instructions.push(inst);
        self
    }
    
    /// Add multiple custom instructions
    pub fn add_instructions(&mut self, insts: Vec<AsmInst>) -> &mut Self {
        assert!(self.prologue_emitted, "Must emit prologue first");
        assert!(!self.epilogue_emitted, "Cannot add instructions after epilogue");
        
        self.instructions.extend(insts);
        self
    }
    
    /// End the function with a return
    /// 
    /// Automatically emits epilogue
    pub fn end_function(&mut self, return_value: Option<(Reg, Option<Reg>)>) -> &mut Self {
        info!("Ending function with return value: {return_value:?}");
        assert!(self.prologue_emitted, "Must emit prologue before epilogue");
        assert!(!self.epilogue_emitted, "Epilogue already emitted");
        assert!(self.cleanup_stack.is_empty(), "Unclosed call sequences");
        
        let ret_insts = self.func.emit_return(return_value);
        trace!("  Return and epilogue generated {} instructions", ret_insts.len());
        self.instructions.extend(ret_insts);
        self.epilogue_emitted = true;
        debug!("Function complete");
        self
    }
    
    /// Build the final instruction list
    /// 
    /// Consumes the builder and returns all instructions
    pub fn build(self) -> Vec<AsmInst> {
        assert!(self.prologue_emitted, "Function must have a prologue");
        assert!(self.epilogue_emitted, "Function must have an epilogue");
        info!("Building function: {} total instructions", self.instructions.len());
        self.instructions
    }
    
    // NO public access to internals - that would defeat the encapsulation!
    // If users need something, we add a proper safe method for it.
}

#[cfg(test)]
mod tests {
    use super::*;
    
    #[test]
    fn test_simple_function() {
        let mut builder = FunctionBuilder::new();
        
        builder
            .begin_function(5)  // 5 local slots
            .load_local(0, Reg::A0)
            .store_local(1, Reg::A0)
            .end_function(Some((Reg::A0, None)));
            
        let instructions = builder.build();
        
        assert!(!instructions.is_empty());
        // Should have R13 initialization
        // Stack bank is initialized in crt0.asm, not in function prologue
    }
    
    #[test]
    fn test_function_with_call() {
        let mut builder = FunctionBuilder::new();
        
        builder.begin_function(3);
        
        // Make a call
        let (ret_val, _) = builder.call_function(
            0x100, 
            2, 
            vec![CallArg::Scalar(Reg::A0)],
            false
        );
        
        // Return the result
        builder.end_function(Some((ret_val, None)));
        
        let instructions = builder.build();
        assert!(!instructions.is_empty());
    }
    
    #[test]
    #[should_panic(expected = "Must emit prologue")]
    fn test_cannot_load_param_without_prologue() {
        let mut builder = FunctionBuilder::new();
        builder.load_parameter(0);
    }
    
    #[test]
    #[should_panic(expected = "Cannot add instructions after epilogue")]
    fn test_cannot_add_after_epilogue() {
        let mut builder = FunctionBuilder::new();
        builder
            .begin_function(0)
            .end_function(None)
            .add_instruction(AsmInst::Add(Reg::R0, Reg::R0, Reg::R0)); // Should panic
    }
    
    #[test]
    #[should_panic(expected = "Unclosed call sequences")]
    fn test_unclosed_call_sequence() {
        let mut builder = FunctionBuilder::new();
        builder
            .begin_function(0)
            .begin_call(vec![CallArg::Scalar(Reg::A0)])
            .emit_call(0x100, 0)
            // Forgot to call end_call()!
            .end_function(None); // Should panic
    }
}