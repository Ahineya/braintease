//! Internal Implementation for FunctionBuilder

#![allow(dead_code)]
//! 
//! THIS MODULE IS INTERNAL TO THE FUNCTION MODULE AND SHOULD NOT BE USED DIRECTLY!
//! All function building should go through FunctionBuilder.
//! 
//! This module contains the internal `FunctionLowering` struct that handles:
//! - Prologue/epilogue generation
//! - Local variable management
//! - Stack frame setup
//! - Register pressure management
//! 
//! The separation between this and FunctionBuilder allows:
//! - Clean public API in FunctionBuilder
//! - Internal complexity hidden here
//! - Easy testing of individual components

use rcc_codegen::{AsmInst, Reg};
use crate::regmgmt::{RegisterPressureManager, BankInfo};
use crate::naming::{NameGenerator, new_function_naming};
use log::{debug, trace, info};

pub(super) struct FunctionLowering {
    pub(super) pressure_manager: RegisterPressureManager,
    pub(super) naming: NameGenerator,
    pub(super) local_count: i16,
    pub(super) spill_count: i16,
    pub(super) instructions: Vec<AsmInst>,
}

impl FunctionLowering {
    /// Create a new function lowering with automatically generated unique naming
    pub(super) fn new() -> Self {
        debug!("Creating new FunctionLowering with unique naming context");
        let naming = new_function_naming();
        
        Self {
            pressure_manager: RegisterPressureManager::new(0),
            naming,
            local_count: 0,
            spill_count: 0,
            instructions: Vec::new(),
        }
    }
    
    /// Create a new function lowering with external pressure manager and naming
    /// This is useful when integrating with existing lowering context
    pub(super) fn with_external_context(
        pressure_manager: RegisterPressureManager,
        naming: NameGenerator,
    ) -> Self {
        debug!("Creating FunctionLowering with external context");
        
        Self {
            pressure_manager,
            naming,
            local_count: 0,
            spill_count: 0,
            instructions: Vec::new(),
        }
    }
    
    /// Helper to setup call arguments using this function's context
    pub(super) fn setup_call(&mut self, args: Vec<super::calling_convention::CallArg>) -> Vec<AsmInst> {
        debug!("Setting up {} call arguments", args.len());
        let cc = super::calling_convention::CallingConvention::new();
        let insts = cc.setup_call_args(&mut self.pressure_manager, &mut self.naming, args, None);
        trace!("  Call setup generated {} instructions", insts.len());
        insts
    }
    
    /// Helper to handle return value using this function's context
    pub(super) fn handle_call_return(&mut self, is_pointer: bool) -> (Vec<AsmInst>, (Reg, Option<Reg>)) {
        debug!("Handling call return value (is_pointer: {is_pointer})");
        let cc = super::calling_convention::CallingConvention::new();
        // For internal use in FunctionBuilder, we don't have a specific result name
        // The builder will handle binding later
        let (insts, regs) = cc.handle_return_value(
            &mut self.pressure_manager, 
            &mut self.naming, 
            is_pointer,
            false,
            false,
            None  // No specific result name - builder handles this
        );
        trace!("  Return handling generated {} instructions", insts.len());
        // We need to return the registers even if None was returned
        // Use Rv0/Rv1 directly since that's where the values are
        let return_regs = regs.unwrap_or({
            if is_pointer {
                (Reg::Rv0, Some(Reg::Rv1))
            } else {
                (Reg::Rv0, None)
            }
        });
        (insts, return_regs)
    }
    
    /// Helper to load a parameter using this function's context
    /// param_types: The types of all parameters to calculate correct stack offsets
    pub(super) fn load_param(&mut self, index: usize, param_types: &[(rcc_common::TempId, rcc_frontend::ir::IrType)]) -> (Vec<AsmInst>, Reg, Option<Reg>) {
        debug!("Loading parameter {index}");
        let cc = super::calling_convention::CallingConvention::new();
        let result = cc.load_param(index, param_types, &mut self.pressure_manager, &mut self.naming);
        trace!("  Parameter load generated {} instructions, result in {:?}, bank in {:?}", result.0.len(), result.1, result.2);
        result
    }
    
    /// Generate function prologue
    /// First 4 parameters are in A0-A3, additional parameters are on stack
    /// NOTE: For simplicity, we always save all callee-saved registers (S0-S3)
    /// A smarter implementation would only save the ones actually used
    pub(super) fn emit_prologue(&mut self, local_slots: i16) -> Vec<AsmInst> {
        info!("Emitting function prologue with {local_slots} local slots");
        let mut insts = Vec::new();
        
        insts.push(AsmInst::Comment("=== Function Prologue ===".to_string()));
        
        // Initialize pressure manager with local count
        // This will handle R13 initialization automatically
        debug!("  Initializing pressure manager for {local_slots} locals");
        self.pressure_manager = RegisterPressureManager::new(local_slots);
        self.pressure_manager.init();
        
        // Take any initialization instructions (R13 setup)
        let init_insts = self.pressure_manager.take_instructions();
        if !init_insts.is_empty() {
            trace!("  R13 initialization generated {} instructions", init_insts.len());
        }
        insts.extend(init_insts);
        
        // Save RA at current SP
        trace!("  Saving RA at SP");
        insts.push(AsmInst::Comment("Save RA at SP".to_string()));
        insts.push(AsmInst::Store(Reg::Ra, Reg::Sb, Reg::Sp));
        insts.push(AsmInst::AddI(Reg::Sp, Reg::Sp, 1));
        
        // Save old FP
        trace!("  Saving old FP");
        insts.push(AsmInst::Comment("Save old FP".to_string()));
        insts.push(AsmInst::Store(Reg::Fp, Reg::Sb, Reg::Sp));
        insts.push(AsmInst::AddI(Reg::Sp, Reg::Sp, 1));
        
        // Save callee-saved registers (S0-S3)
        // For simplicity, we always save all of them
        // A smarter implementation would track which ones are actually used
        insts.push(AsmInst::Comment("Save callee-saved registers S0-S3".to_string()));
        for reg in [Reg::S0, Reg::S1, Reg::S2, Reg::S3] {
            trace!("  Saving {reg:?}");
            insts.push(AsmInst::Store(reg, Reg::Sb, Reg::Sp));
            insts.push(AsmInst::AddI(Reg::Sp, Reg::Sp, 1));
        }
        
        // Set new FP = SP
        trace!("  Setting FP = SP");
        insts.push(AsmInst::Comment("Set FP = SP".to_string()));
        insts.push(AsmInst::Add(Reg::Fp, Reg::Sp, Reg::R0));
        
        // Allocate space for locals
        if local_slots > 0 {
            debug!("  Allocating {local_slots} stack slots for locals");
            insts.push(AsmInst::Comment(format!("Allocate {local_slots} slots for locals")));
            insts.push(AsmInst::AddI(Reg::Sp, Reg::Sp, local_slots));
        } else {
            trace!("  No local slots to allocate");
        }

        // Reserve extra space for register spills so FP+local_count+slot stays within frame
        const RESERVED_SPILL_SLOTS: i16 = 20;
        insts.push(AsmInst::Comment(format!(
            "Reserve {RESERVED_SPILL_SLOTS} spill slots above locals"
        )));
        insts.push(AsmInst::AddI(Reg::Sp, Reg::Sp, RESERVED_SPILL_SLOTS));
        
        self.local_count = local_slots;
        
        debug!("Prologue complete: generated {} instructions", insts.len());
        insts
    }
    
    /// Generate function epilogue
    pub(super) fn emit_epilogue(&mut self) -> Vec<AsmInst> {
        info!("Emitting function epilogue");
        let mut insts = Vec::new();
        
        insts.push(AsmInst::Comment("=== Function Epilogue ===".to_string()));
        
        // Restore SP = FP
        trace!("  Restoring SP = FP");
        insts.push(AsmInst::Comment("Restore SP = FP".to_string()));
        insts.push(AsmInst::Add(Reg::Sp, Reg::Fp, Reg::R0));
        
        // Restore callee-saved registers (S3-S0 in reverse order)
        // We saved RA, FP, S0, S1, S2, S3 in that order
        // So FP-1 = S3, FP-2 = S2, FP-3 = S1, FP-4 = S0, FP-5 = old FP, FP-6 = RA
        insts.push(AsmInst::Comment("Restore callee-saved registers S3-S0".to_string()));
        for (offset, reg) in [(-1, Reg::S3), (-2, Reg::S2), (-3, Reg::S1), (-4, Reg::S0)] {
            trace!("  Restoring {reg:?} from FP{offset}");
            insts.push(AsmInst::AddI(Reg::Sc, Reg::Fp, offset));
            insts.push(AsmInst::Load(reg, Reg::Sb, Reg::Sc));
        }
        
        // Restore old FP (at FP-5, which is now SP-5)
        trace!("  Restoring old FP");
        insts.push(AsmInst::Comment("Restore old FP".to_string()));
        insts.push(AsmInst::AddI(Reg::Sp, Reg::Sp, -5));
        insts.push(AsmInst::Load(Reg::Fp, Reg::Sb, Reg::Sp));
        
        // Restore RA (at FP-6, which is now SP-1) 
        trace!("  Restoring RA");
        insts.push(AsmInst::Comment("Restore RA".to_string()));
        insts.push(AsmInst::AddI(Reg::Sp, Reg::Sp, -1));
        insts.push(AsmInst::Load(Reg::Ra, Reg::Sb, Reg::Sp));
        
        // Return to caller
        // Need to restore PCB from RAB for cross-bank returns
        // Then JALR to RA
        debug!("  Setting up return to caller");
        insts.push(AsmInst::Comment("Return to caller".to_string()));
        
        // Restore the caller's bank (RAB was saved by JAL)
        // Note: RAB is automatically saved by JAL instruction
        // but we need to explicitly restore PCB before jumping back
        trace!("  Restoring PCB from RAB");
        insts.push(AsmInst::Add(Reg::Pcb, Reg::Rab, Reg::R0));
        
        // JALR rd, 0, rs format: rd←PC+1 (unused), PC←rs
        // This jumps to the address in RA within the bank we just restored
        trace!("  JALR to RA");
        insts.push(AsmInst::Jalr(Reg::R0, Reg::R0, Reg::Ra));
        
        debug!("Epilogue complete: generated {} instructions", insts.len());
        insts
    }
    
    /// Handle return statement
    pub(super) fn emit_return(&mut self, value: Option<(Reg, Option<Reg>)>) -> Vec<AsmInst> {
        debug!("Emitting return statement");
        let mut insts = Vec::new();
        
        if let Some((addr_reg, bank_reg)) = value {
            // Return value or fat pointer
            if let Some(bank) = bank_reg {
                // Fat pointer return: R3=addr, R4=bank
                info!("  Returning fat pointer: addr={addr_reg:?}, bank={bank:?}");
                insts.push(AsmInst::Comment("Return fat pointer".to_string()));
                if addr_reg != Reg::Rv0 {
                    trace!("  Moving address from {addr_reg:?} to R3");
                    insts.push(AsmInst::Add(Reg::Rv0, addr_reg, Reg::R0));
                } else {
                    trace!("  Address already in R3");
                }
                if bank != Reg::Rv1 {
                    trace!("  Moving bank from {bank:?} to R4");
                    insts.push(AsmInst::Add(Reg::Rv1, bank, Reg::R0));
                } else {
                    trace!("  Bank already in R4");
                }
            } else {
                // Scalar return: R3=value
                info!("  Returning scalar value in {addr_reg:?}");
                insts.push(AsmInst::Comment("Return scalar value".to_string()));
                if addr_reg != Reg::Rv0 {
                    trace!("  Moving value from {addr_reg:?} to R3");
                    insts.push(AsmInst::Add(Reg::Rv0, addr_reg, Reg::R0));
                } else {
                    trace!("  Value already in R3");
                }
            }
        } else {
            debug!("  Void return (no value)");
        }
        
        // Add epilogue
        let epilogue = self.emit_epilogue();
        trace!("  Adding epilogue ({} instructions)", epilogue.len());
        insts.extend(epilogue);
        
        debug!("Return complete: generated {} instructions", insts.len());
        insts
    }
    
    /// Get local variable address
    pub(super) fn get_local_addr(&mut self, offset: i16) -> Reg {
        debug!("Getting address of local variable at FP+{offset}");
        
        // Use pressure manager for better register allocation
        let local_name = self.naming.local_name(offset);
        trace!("  Allocating register for local '{local_name}'");
        let reg = self.pressure_manager.get_register(local_name.clone());
        
        self.instructions.push(AsmInst::Comment(format!("Get address of local at FP+{offset}")));
        trace!("  Computing address: FP + {offset}");
        self.instructions.push(AsmInst::Add(reg, Reg::Fp, Reg::R0));
        self.instructions.push(AsmInst::AddI(reg, reg, offset));
        
        // Take any spill/reload instructions generated
        let spill_insts = self.pressure_manager.take_instructions();
        if !spill_insts.is_empty() {
            debug!("  Generated {} spill/reload instructions", spill_insts.len());
        }
        self.instructions.extend(spill_insts);
        
        // Mark this as a stack pointer
        trace!("  Marking '{local_name}' as stack pointer");
        self.pressure_manager.set_pointer_bank(local_name, 
                                        BankInfo::Stack);
        
        debug!("Local address in register {reg:?}");
        reg
    }
    
    /// Load from local variable
    pub(super) fn load_local(&mut self, offset: i16, dest: Reg) -> Vec<AsmInst> {
        info!("Loading from local variable at FP+{offset} into {dest:?}");
        let mut insts = Vec::new();
        
        insts.push(AsmInst::Comment(format!("Load from local at FP+{offset}")));
        
        // Calculate address using pressure manager
        let addr_name = self.naming.local_addr_name(offset);
        trace!("  Allocating temporary register for address calculation ({addr_name})");
        let addr_reg = self.pressure_manager.get_register(addr_name);
        
        let spill_insts = self.pressure_manager.take_instructions();
        if !spill_insts.is_empty() {
            debug!("  Generated {} spill/reload instructions", spill_insts.len());
        }
        insts.extend(spill_insts);
        
        trace!("  Computing address: FP + {offset}");
        insts.push(AsmInst::Add(addr_reg, Reg::Fp, Reg::R0));
        insts.push(AsmInst::AddI(addr_reg, addr_reg, offset));
        
        // Load using R13 as bank
        debug!("  Loading from stack (bank R13) at address in {addr_reg:?} to {dest:?}");
        insts.push(AsmInst::Load(dest, Reg::Sb, addr_reg));
        
        trace!("  Freeing temporary address register {addr_reg:?}");
        self.pressure_manager.free_register(addr_reg);
        
        debug!("Load complete: generated {} instructions", insts.len());
        insts
    }
    
    /// Store to local variable
    pub(super) fn store_local(&mut self, offset: i16, src: Reg) -> Vec<AsmInst> {
        info!("Storing {src:?} to local variable at FP+{offset}");
        let mut insts = Vec::new();
        
        insts.push(AsmInst::Comment(format!("Store to local at FP+{offset}")));
        
        // Calculate address using pressure manager
        let addr_name = self.naming.local_addr_name(offset);
        trace!("  Allocating temporary register for address calculation ({addr_name})");
        let addr_reg = self.pressure_manager.get_register(addr_name);
        
        let spill_insts = self.pressure_manager.take_instructions();
        if !spill_insts.is_empty() {
            debug!("  Generated {} spill/reload instructions", spill_insts.len());
        }
        insts.extend(spill_insts);
        
        trace!("  Computing address: FP + {offset}");
        insts.push(AsmInst::Add(addr_reg, Reg::Fp, Reg::R0));
        insts.push(AsmInst::AddI(addr_reg, addr_reg, offset));
        
        // Store using R13 as bank
        debug!("  Storing {src:?} to stack (bank R13) at address in {addr_reg:?}");
        insts.push(AsmInst::Store(src, Reg::Sb, addr_reg));
        
        trace!("  Freeing temporary address register {addr_reg:?}");
        self.pressure_manager.free_register(addr_reg);
        
        debug!("Store complete: generated {} instructions", insts.len());
        insts
    }
}

// Tests moved to tests/function_tests.rs