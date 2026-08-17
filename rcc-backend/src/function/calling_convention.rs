//! Correct Calling Convention Implementation
//! 
//! Key fixes from v1:
//! - Parameters passed on STACK (as per spec, not in registers!)
//! - Return values: R3 for scalar/addr, R4 for bank
//! - Proper fat pointer handling
//! - Stack parameters pushed before call, accessed via FP in callee

use rcc_codegen::{AsmInst, Reg};
use crate::regmgmt::{BankInfo, RegisterPressureManager};
use log::{debug, trace, info};
use crate::naming::NameGenerator;

/// Target for function calls - either by address or by label
#[derive(Debug, Clone)]
pub enum CallTarget {
    /// Direct call to a known address with bank
    Address { addr: u16, bank: u16 },
    /// Call to a function by label (will be resolved by assembler)
    Label(String),
}

pub(super) struct CallingConvention {}

impl Default for CallingConvention {
    fn default() -> Self {
        Self::new()
    }
}

impl CallingConvention {
    pub(super) fn new() -> Self {
        Self {}
    }
    
    /// Core logic for analyzing parameter/argument placement
    /// Takes a closure that determines if an item is a fat pointer
    /// Returns (register_items_with_slots, first_stack_item_index)
    fn analyze_placement<F>(&self, count: usize, slots: F) -> (Vec<(usize, usize)>, usize)
    where
        F: Fn(usize) -> usize,
    {
        let mut register_items = Vec::new();
        let mut reg_slots_used = 0;
        let mut first_stack_item = count;
        
        for i in 0..count {
            let n = slots(i);
            if n == 0 {
                continue;
            }
            if reg_slots_used + n <= 4 {
                register_items.push((i, reg_slots_used));
                reg_slots_used += n;
            } else {
                first_stack_item = i;
                break;
            }
        }
        
        (register_items, first_stack_item)
    }
    
    /// Analyze which arguments go in registers vs stack based on CallArg types
    fn analyze_arg_placement(&self, args: &[CallArg], stack_args_from: Option<usize>) -> (Vec<(usize, usize)>, usize) {
        let (mut register_items, mut first_stack_item) =
            self.analyze_placement(args.len(), |i| match args[i] {
                CallArg::Scalar(_) => 1,
                CallArg::FatPointer { .. } => 2,
                CallArg::I64 { .. } => 4,
            });
        if let Some(from) = stack_args_from {
            register_items.retain(|(i, _)| *i < from);
            first_stack_item = first_stack_item.min(from);
        }
        (register_items, first_stack_item)
    }
    
    /// Analyze parameter placement for load_param based on IrType
    fn analyze_param_placement(&self, param_types: &[(rcc_common::TempId, rcc_frontend::ir::IrType)])
        -> (Vec<(usize, usize)>, usize) {
        self.analyze_placement(param_types.len(), |i| param_types[i].1.abi_slots())
    }
    
    /// Calculate the stack offset for a parameter that's passed on the stack
    /// This centralizes the offset calculation logic to avoid duplication
    /// 
    /// # Arguments
    /// * `param_index` - The index of the parameter we're calculating the offset for
    /// * `param_types` - All parameter types to properly account for fat pointers
    /// * `first_stack_param` - The index of the first parameter that goes on the stack
    /// 
    /// # Returns
    /// The offset from FP to access this parameter (will be negative)
    fn calculate_stack_param_offset(
        &self,
        param_index: usize,
        param_types: &[(rcc_common::TempId, rcc_frontend::ir::IrType)],
        first_stack_param: usize,
    ) -> i16 {
        // Stack layout: ... param6, param5, param4, RA, FP, S0, S1, S2, S3, locals...
        // Start at -6 for FP, RA, and S0-S3
        let mut offset = -6i16;
        
        // Count stack words before our parameter
        for i in first_stack_param..param_index {
            if i >= param_types.len() {
                break;
            }
            offset -= param_types[i].1.abi_slots() as i16;
        }
        
        // Account for this parameter itself
        offset -= 1;
        
        offset
    }

    /// FP-relative offset of the first unnamed (variadic) argument.
    /// Extra args are always on the stack, after any named stack parameters.
    pub(super) fn first_vararg_offset(
        &self,
        named_params: &[(rcc_common::TempId, rcc_frontend::ir::IrType)],
    ) -> i16 {
        let (_, first_stack_param) = self.analyze_param_placement(named_params);
        let mut dummy = named_params.to_vec();
        dummy.push((u32::MAX, rcc_frontend::ir::IrType::I16));
        self.calculate_stack_param_offset(named_params.len(), &dummy, first_stack_param)
    }
    
    /// Setup parameters for a function call
    /// First 4 scalar arguments or 2 fat pointers go in A0-A3
    /// Remaining arguments are passed on the stack
    /// 
    /// IMPORTANT: This function now handles spilling automatically
    /// It will spill all live registers before setting up arguments to prevent clobbering
    pub(super) fn setup_call_args(&self, 
                           pressure_manager: &mut RegisterPressureManager,
                           _naming: &mut NameGenerator,
                           args: Vec<CallArg>,
                           stack_args_from: Option<usize>) -> Vec<AsmInst> {
        info!("Setting up {} call arguments", args.len());
        let mut insts = Vec::new();
        let mut stack_offset = 0i16;
        let arg_regs = [Reg::A0, Reg::A1, Reg::A2, Reg::A3];
        
        // CRITICAL: Spill all live registers before setting up arguments
        // This prevents source registers from being clobbered during argument setup
        debug!("  Spilling all live registers before call");
        insts.push(AsmInst::Comment("Spill live registers before call".to_string()));
        pressure_manager.spill_all();
        let spill_insts = pressure_manager.take_instructions();
        if !spill_insts.is_empty() {
            trace!("  Generated {} spill instructions", spill_insts.len());
            insts.extend(spill_insts);
        }

        // Spills are FP-relative. Raise SP to at least the prologue frame
        // (locals + reserved spill slots) and grow further if we used more
        // spill slots, so the callee cannot overlap live spills.
        let reserved = pressure_manager.local_count() + 20;
        let frame_top = pressure_manager.frame_high_water().max(reserved);
        insts.push(AsmInst::Comment(format!(
            "Set SP = FP+{frame_top} so callee frame is above spills"
        )));
        insts.push(AsmInst::AddI(Reg::Sp, Reg::Fp, frame_top));
        
        // Use common logic to determine placement
        let (register_arg_slots, _first_stack_arg) = self.analyze_arg_placement(&args, stack_args_from);
        
        // Separate args into register args and stack args
        let mut reg_args = Vec::new();
        let mut stack_args = Vec::new();
        
        for (idx, arg) in args.into_iter().enumerate() {
            if let Some((_, reg_slot)) = register_arg_slots.iter().find(|(i, _)| *i == idx) {
                // This arg goes in register(s)
                match &arg {
                    CallArg::Scalar(_) => {
                        trace!("  Arg {} (scalar) goes in {:?}", idx, arg_regs[*reg_slot]);
                        reg_args.push((idx, arg_regs[*reg_slot], arg));
                    }
                    CallArg::FatPointer { .. } => {
                        trace!("  Arg {} (fat ptr) goes in {:?} and {:?}", idx, 
                               arg_regs[*reg_slot], arg_regs[*reg_slot + 1]);
                        reg_args.push((idx, arg_regs[*reg_slot], arg));
                    }
                    CallArg::I64 { .. } => {
                        trace!("  Arg {} (i64) goes in A0-A3", idx);
                        reg_args.push((idx, arg_regs[*reg_slot], arg));
                    }
                }
            } else {
                // This arg goes on stack
                match &arg {
                    CallArg::Scalar(_) => trace!("  Arg {idx} (scalar) goes on stack"),
                    CallArg::FatPointer { .. } => trace!("  Arg {idx} (fat ptr) goes on stack"),
                    CallArg::I64 { .. } => trace!("  Arg {idx} (i64) goes on stack"),
                }
                stack_args.push((idx, arg));
            }
        }
        
        // First, push stack arguments in reverse order
        if !stack_args.is_empty() {
            insts.push(AsmInst::Comment(format!("Pushing {} arguments to stack", stack_args.len())));
            debug!("  Pushing {} stack arguments in reverse order", stack_args.len());
            
            for (idx, arg) in stack_args.into_iter().rev() {
                match arg {
                    CallArg::Scalar(src_reg) => {
                        insts.push(AsmInst::Comment(format!("Push arg {idx} (scalar) to stack")));
                        insts.push(AsmInst::Store(src_reg, Reg::Sb, Reg::Sp));
                        insts.push(AsmInst::AddI(Reg::Sp, Reg::Sp, 1));
                        stack_offset += 1;
                    }
                    CallArg::FatPointer { addr, bank } => {
                        insts.push(AsmInst::Comment(format!("Push arg {idx} (fat ptr) to stack")));
                        insts.push(AsmInst::Store(bank, Reg::Sb, Reg::Sp));
                        insts.push(AsmInst::AddI(Reg::Sp, Reg::Sp, 1));
                        insts.push(AsmInst::Store(addr, Reg::Sb, Reg::Sp));
                        insts.push(AsmInst::AddI(Reg::Sp, Reg::Sp, 1));
                        stack_offset += 2;
                    }
                    CallArg::I64 { words } => {
                        insts.push(AsmInst::Comment(format!("Push arg {idx} (i64) to stack")));
                        for w in words.iter().rev() {
                            insts.push(AsmInst::Store(*w, Reg::Sb, Reg::Sp));
                            insts.push(AsmInst::AddI(Reg::Sp, Reg::Sp, 1));
                        }
                        stack_offset += 4;
                    }
                }
            }
        }

        // Then, move register arguments to A0-A3
        if !reg_args.is_empty() {
            insts.push(AsmInst::Comment(format!("Setting up {} register arguments", reg_args.len())));
            debug!("  Setting up {} register arguments", reg_args.len());
            
            for (idx, dest_reg, arg) in reg_args {
                match arg {
                    CallArg::Scalar(src_reg) => {
                        insts.push(AsmInst::Comment(format!("Arg {idx} (scalar) to {dest_reg:?}")));
                        if src_reg != dest_reg {
                            insts.push(AsmInst::Add(dest_reg, src_reg, Reg::R0));
                        }
                    }
                    CallArg::FatPointer { addr, bank } => {
                        // Fat pointer uses two consecutive registers
                        let bank_reg = match dest_reg {
                            Reg::A0 => Reg::A1,
                            Reg::A1 => Reg::A2,
                            Reg::A2 => Reg::A3,
                            _ => panic!("Invalid fat pointer register assignment"),
                        };
                        insts.push(AsmInst::Comment(format!("Arg {idx} (fat ptr) to {dest_reg:?},{bank_reg:?}")));
                        if addr != dest_reg {
                            insts.push(AsmInst::Add(dest_reg, addr, Reg::R0));
                        }
                        if bank != bank_reg {
                            insts.push(AsmInst::Add(bank_reg, bank, Reg::R0));
                        }
                    }
                    CallArg::I64 { words } => {
                        let dests = [Reg::A0, Reg::A1, Reg::A2, Reg::A3];
                        insts.push(AsmInst::Comment(format!("Arg {idx} (i64) to A0-A3")));
                        for i in 0..4 {
                            if words[i] != dests[i] {
                                insts.push(AsmInst::Add(dests[i], words[i], Reg::R0));
                            }
                        }
                    }
                }
            }
        }

        if stack_offset > 0 {
            insts.push(AsmInst::Comment(format!("Pushed {stack_offset} words to stack")));
        }
        debug!("Call args setup complete: {} in registers, {} words on stack, {} total instructions", 
               register_arg_slots.len(), stack_offset, insts.len());
        
        insts
    }
    
    /// Generate call instruction
    /// For cross-bank calls, sets PCB first then uses JAL
    pub(super) fn emit_call(&self, func_addr: u16, func_bank: u16) -> Vec<AsmInst> {
        info!("Emitting call to function at bank:{func_bank}, addr:{func_addr}");
        let mut insts = Vec::new();
        
        insts.push(AsmInst::Comment(format!("Call function at bank:{func_bank}, addr:{func_addr}")));
        
        // JAL only jumps within current bank, saving RA/RAB
        // For cross-bank calls, we need to set PCB first
        if func_bank != 0 {
            debug!("  Cross-bank call: setting PCB to {func_bank}");
            insts.push(AsmInst::Comment("Set PCB for cross-bank call".to_string()));
            insts.push(AsmInst::Li(Reg::Pcb, func_bank as i16));
        } else {
            trace!("  In-bank call (bank 0)");
        }
        
        // JAL addr - sets RA←PC+1, RAB←PCB, PC←addr
        // The actual instruction is: JAL RA, R0, addr (RA is implicit)
        // Our AsmInst::Jal(bank, addr) abstraction will be lowered to proper format
        // First param is traditionally bank but for in-bank jumps it's 0
        trace!("  JAL instruction: saves RA←PC+1, RAB←PCB, jumps to addr {func_addr}");
        insts.push(AsmInst::Jal(0, func_addr as i16));
        
        debug!("Call emission complete: generated {} instructions", insts.len());
        insts
    }
    
    /// Handle return value after call
    /// Binds the return value to the specified result name in the register manager
    pub(super) fn handle_return_value(&self, 
                              pressure_manager: &mut RegisterPressureManager,
                              naming: &mut NameGenerator,
                              is_pointer: bool,
                              is_wide: bool,
                              is_i64: bool,
                              result_name: Option<String>) -> (Vec<AsmInst>, Option<(Reg, Option<Reg>)>) {
        let mut insts = Vec::new();
        
        if let Some(name) = result_name {
            if is_pointer {
                // Fat pointer return in Rv0 (addr) and Rv1 (bank)
                debug!("Handling fat pointer return for '{name}'");
                insts.push(AsmInst::Comment(format!("Fat pointer return value for {name}")));
                
                // Bind Rv0 to the result name
                pressure_manager.bind_value_to_register(name.clone(), Reg::Rv0);
                
                // Track that Rv1 has the bank
                pressure_manager.set_pointer_bank(name, BankInfo::Register(Reg::Rv1));
                insts.extend(pressure_manager.take_instructions());
                
                (insts, Some((Reg::Rv0, Some(Reg::Rv1))))
            } else if is_i64 {
                debug!("Handling I64 return for '{name}'");
                insts.push(AsmInst::Comment(format!("I64 return value for {name}")));
                pressure_manager.bind_value_to_register(name.clone(), Reg::Rv0);
                let w1 = naming.i64_word_name(&name, 1);
                let w2 = naming.i64_word_name(&name, 2);
                let w3 = naming.i64_word_name(&name, 3);
                pressure_manager.bind_value_to_register(w1.clone(), Reg::Rv1);
                pressure_manager.bind_value_to_register(w2.clone(), Reg::X0);
                pressure_manager.bind_value_to_register(w3.clone(), Reg::X1);
                pressure_manager.set_i64_words(name, [w1, w2, w3]);
                insts.extend(pressure_manager.take_instructions());
                (insts, Some((Reg::Rv0, Some(Reg::Rv1))))
            } else if is_wide {
                debug!("Handling I32 return for '{name}'");
                insts.push(AsmInst::Comment(format!("I32 return value for {name}")));
                pressure_manager.bind_value_to_register(name.clone(), Reg::Rv0);
                let hi_name = naming.i32_high_name(&name);
                pressure_manager.bind_value_to_register(hi_name.clone(), Reg::Rv1);
                pressure_manager.set_i32_high(name, hi_name);
                insts.extend(pressure_manager.take_instructions());
                (insts, Some((Reg::Rv0, Some(Reg::Rv1))))
            } else {
                // Scalar return in Rv0
                debug!("Handling scalar return for '{name}'");
                insts.push(AsmInst::Comment(format!("Scalar return value for {name}")));
                
                // Bind Rv0 to the result name
                pressure_manager.bind_value_to_register(name, Reg::Rv0);
                insts.extend(pressure_manager.take_instructions());
                
                (insts, Some((Reg::Rv0, None)))
            }
        } else {
            // No return value (void function)
            debug!("No return value (void function)");
            (insts, None)
        }
    }
    
    /// Clean up stack after call
    pub(super) fn cleanup_stack(&self, num_args_words: i16) -> Vec<AsmInst> {
        let mut insts = Vec::new();
        if num_args_words > 0 {
            debug!("Cleaning up {num_args_words} words from stack after call");
            insts.push(AsmInst::Comment(format!("Clean up {num_args_words} words from stack")));
            insts.push(AsmInst::AddI(Reg::Sp, Reg::Sp, -num_args_words));
            trace!("  Adjusted SP by -{num_args_words}");
        } else {
            trace!("No stack cleanup needed (0 arguments)");
        }
        insts
    }
    
    /// Complete function call sequence including setup, call, return handling, and cleanup
    /// This is the main entry point for making function calls in the lowering code
    /// 
    /// # Arguments
    /// * `target` - Either a direct address or a label for the function to call
    /// * `args` - Arguments to pass to the function
    /// * `returns_pointer` - Whether the function returns a fat pointer
    /// * `result_name` - Optional name for the return value binding
    pub(super) fn make_complete_call(
        &self,
        pressure_manager: &mut RegisterPressureManager,
        naming: &mut NameGenerator,
        target: CallTarget,
        args: Vec<CallArg>,
        returns_pointer: bool,
        returns_wide: bool,
        returns_i64: bool,
        result_name: Option<String>,
        stack_args_from: Option<usize>,
    ) -> (Vec<AsmInst>, Option<(Reg, Option<Reg>)>) {
        match &target {
            CallTarget::Address { addr, bank } => {
                info!("Making complete call to function at bank:{}, addr:{} with {} args", 
                      bank, addr, args.len());
            }
            CallTarget::Label(label) => {
                info!("Making complete call to function '{}' with {} args", label, args.len());
            }
        }
        
        let mut insts = Vec::new();
        
        // Calculate stack words needed for cleanup
        // Only count arguments that will be pushed to stack, not register arguments
        let (register_arg_slots, _) = self.analyze_arg_placement(&args, stack_args_from);
        let mut stack_words = 0i16;
        for (idx, arg) in args.iter().enumerate() {
            // Skip arguments that go in registers
            if register_arg_slots.iter().any(|(i, _)| *i == idx) {
                continue;
            }
            // Count stack words for this argument
            stack_words += match arg {
                CallArg::Scalar(_) => 1,
                CallArg::FatPointer { .. } => 2,
                CallArg::I64 { .. } => 4,
            };
        }
        debug!("  Call will use {stack_words} stack words");
        
        // 1. Setup arguments
        trace!("  Setting up call arguments");
        let setup = self.setup_call_args(pressure_manager, naming, args, stack_args_from);
        insts.extend(setup);
        
        // 2. Emit call instruction
        trace!("  Emitting call instruction");
        match target {
            CallTarget::Address { addr, bank } => {
                let call = self.emit_call(addr, bank);
                insts.extend(call);
            }
            CallTarget::Label(label) => {
                insts.push(AsmInst::Comment(format!("Call function {label}")));
                insts.push(AsmInst::Call(label));
            }
        }
        
        // 3. Handle return value (if any)
        let (ret_insts, return_regs) = self.handle_return_value(
            pressure_manager, 
            naming, 
            returns_pointer,
            returns_wide,
            returns_i64,
            result_name
        );
        insts.extend(ret_insts);
        
        // 4. Clean up stack
        trace!("  Cleaning up {stack_words} stack words");
        let cleanup = self.cleanup_stack(stack_words);
        insts.extend(cleanup);
        
        debug!("Complete call sequence finished: {} total instructions", insts.len());
        (insts, return_regs)
    }
    
    /// Load parameter in callee
    /// First 4 scalar parameters or two pointer params are in A0-A3
    /// Additional parameters are on the stack at negative offsets from FP
    /// param_types: The types of all parameters to calculate correct stack offsets
    /// Returns (instructions, address_reg, optional_bank_reg for fat pointers)
    pub(super) fn load_param(&self, index: usize, 
                     param_types: &[(rcc_common::TempId, rcc_frontend::ir::IrType)],
                     pressure_manager: &mut RegisterPressureManager,
                     naming: &mut NameGenerator) -> (Vec<AsmInst>, Reg, Option<Reg>) {
        info!("Loading parameter {index}");
        let mut insts = Vec::new();

        let param_name = naming.param_name(index);
        trace!("  Allocating register for parameter '{param_name}'");
        let dest = pressure_manager.get_register(param_name);

        let spill_insts = pressure_manager.take_instructions();
        if !spill_insts.is_empty() {
            trace!("  Generated {} spill/reload instructions", spill_insts.len());
        }
        insts.extend(spill_insts);
        // Use common logic to determine placement
        let (register_params, first_stack_param) = self.analyze_param_placement(param_types);

        // Check if this parameter is in a register
        let param_reg = if let Some((_, reg_slot)) = register_params.iter().find(|(i, _)| *i == index) {
            Some(match *reg_slot {
                0 => Reg::A0,
                1 => Reg::A1,
                2 => Reg::A2,
                3 => Reg::A3,
                _ => unreachable!(),
            })
        } else {
            None
        };

        if let Some(arg_reg) = param_reg {
            // Parameter is in a register
            debug!("  Parameter {index} is in register {arg_reg:?}");
            insts.push(AsmInst::Comment(format!("Load param {index} from {arg_reg:?}")));
            if dest != arg_reg {
                insts.push(AsmInst::Add(dest, arg_reg, Reg::R0));
            }

            if index < param_types.len() && param_types[index].1.is_four_word_scalar() {
                let srcs = [Reg::A0, Reg::A1, Reg::A2, Reg::A3];
                let pn = naming.param_name(index);
                let mut extra = [String::new(), String::new(), String::new()];
                let mut extra_regs = [Reg::R0; 3];
                for i in 0..3 {
                    extra[i] = naming.i64_word_name(&pn, (i + 1) as u8);
                    extra_regs[i] = pressure_manager.get_register(extra[i].clone());
                    insts.extend(pressure_manager.take_instructions());
                    if extra_regs[i] != srcs[i + 1] {
                        insts.push(AsmInst::Add(extra_regs[i], srcs[i + 1], Reg::R0));
                    }
                    pressure_manager.bind_value_to_register(extra[i].clone(), extra_regs[i]);
                }
                pressure_manager.set_i64_words(pn.clone(), extra.clone());
                pressure_manager.set_i64_words(format!("__param_{index}"), extra);
                if param_types[index].1.is_f64() {
                    pressure_manager.set_fp64(pn);
                }
                debug!("Parameter load complete (i64): generated {} instructions", insts.len());
                return (insts, dest, Some(extra_regs[0]));
            }

            // If this is a fat pointer, copy the bank out of A1/A2/A3 into a
            // named register. Those argument registers are otherwise free for
            // the allocator and would clobber the bank tag.
            let bank_reg = if index < param_types.len() && param_types[index].1.takes_two_slots() {
                let src_bank_reg = match arg_reg {
                    Reg::A0 => Reg::A1,
                    Reg::A1 => Reg::A2,
                    Reg::A2 => Reg::A3,
                    _ => panic!("Invalid two-slot register for param {index}"),
                };
                debug!("  Copying param {index} second word from {src_bank_reg:?}");
                insts.push(AsmInst::Comment(format!("Copy param {index} high/bank from {src_bank_reg:?}")));

                let bank_reg_name = if param_types[index].1.is_two_word_scalar() {
                    let pn = naming.param_name(index);
                    naming.i32_high_name(&pn)
                } else {
                    naming.param_bank_name(index)
                };
                let bank_reg = pressure_manager.get_register(bank_reg_name.clone());
                insts.extend(pressure_manager.take_instructions());
                if bank_reg != src_bank_reg {
                    insts.push(AsmInst::Add(bank_reg, src_bank_reg, Reg::R0));
                }
                pressure_manager.bind_value_to_register(bank_reg_name.clone(), bank_reg);

                let param_name = naming.param_name(index);
                if param_types[index].1.is_two_word_scalar() {
                    pressure_manager.set_i32_high(param_name.clone(), bank_reg_name);
                    if param_types[index].1.is_f32() {
                        pressure_manager.set_fp32(param_name);
                    }
                } else {
                    pressure_manager.set_pointer_bank(param_name, BankInfo::Dynamic(bank_reg_name));
                }

                Some(bank_reg)
            } else {
                None
            };

            debug!("Parameter load complete: generated {} instructions, result in {:?}, bank in {:?}",
                   insts.len(), dest, bank_reg);
            return (insts, dest, bank_reg);
        } else {
            // Parameter is on the stack
            // Use the centralized helper to calculate the offset
            let param_offset = self.calculate_stack_param_offset(index, param_types, first_stack_param);
            debug!("  Parameter {index} (stack param) is at FP{param_offset}");

            insts.push(AsmInst::Comment(format!("Load param {index} from FP{param_offset}")));
            trace!("  Computing address: FP + {param_offset}");
            insts.push(AsmInst::AddI(Reg::Sc, Reg::Fp, param_offset));
            trace!("  Loading from stack (bank SB) at computed address into {dest:?}");
            insts.push(AsmInst::Load(dest, Reg::Sb, Reg::Sc));

            if index < param_types.len() && param_types[index].1.is_four_word_scalar() {
                let pn = naming.param_name(index);
                let mut extra = [String::new(), String::new(), String::new()];
                let mut extra_regs = [Reg::R0; 3];
                for i in 0..3 {
                    extra[i] = naming.i64_word_name(&pn, (i + 1) as u8);
                    extra_regs[i] = pressure_manager.get_register(extra[i].clone());
                    insts.extend(pressure_manager.take_instructions());
                    insts.push(AsmInst::AddI(Reg::Sc, Reg::Fp, param_offset - 1 - i as i16));
                    insts.push(AsmInst::Load(extra_regs[i], Reg::Sb, Reg::Sc));
                    pressure_manager.bind_value_to_register(extra[i].clone(), extra_regs[i]);
                }
                pressure_manager.set_i64_words(pn.clone(), extra.clone());
                pressure_manager.set_i64_words(format!("__param_{index}"), extra);
                if param_types[index].1.is_f64() {
                    pressure_manager.set_fp64(pn);
                }
                debug!("Parameter load complete (i64 stack): generated {} instructions", insts.len());
                return (insts, dest, Some(extra_regs[0]));
            }

            // If this is a fat pointer, also load the bank from the next stack slot
            let bank_reg = if index < param_types.len() && param_types[index].1.takes_two_slots() {
                debug!("  Loading second word of param {index} from FP{}", param_offset - 1);
                insts.push(AsmInst::Comment(format!("Load param {index} high/bank from FP{}", param_offset - 1)));

                let bank_reg_name = if param_types[index].1.is_two_word_scalar() {
                    let pn = naming.param_name(index);
                    naming.i32_high_name(&pn)
                } else {
                    naming.param_bank_name(index)
                };
                let bank_reg = pressure_manager.get_register(bank_reg_name.clone());
                insts.extend(pressure_manager.take_instructions());

                insts.push(AsmInst::AddI(Reg::Sc, Reg::Fp, param_offset - 1));
                insts.push(AsmInst::Load(bank_reg, Reg::Sb, Reg::Sc));

                let param_name = naming.param_name(index);
                if param_types[index].1.is_two_word_scalar() {
                    pressure_manager.bind_value_to_register(bank_reg_name.clone(), bank_reg);
                    pressure_manager.set_i32_high(param_name.clone(), bank_reg_name);
                    if param_types[index].1.is_f32() {
                        pressure_manager.set_fp32(param_name);
                    }
                } else {
                    pressure_manager.set_pointer_bank(param_name, BankInfo::Register(bank_reg));
                }

                Some(bank_reg)
            } else {
                None
            };

            debug!("Parameter load complete: generated {} instructions, result in {:?}, bank in {:?}",
                   insts.len(), dest, bank_reg);
            return (insts, dest, bank_reg);
        }
    }
}

/// Argument types for function calls
#[derive(Debug, Clone)]
pub enum CallArg {  // This needs to stay pub for the re-export
    Scalar(Reg),
    FatPointer { addr: Reg, bank: Reg },
    I64 { words: [Reg; 4] },
}

// Tests moved to tests/calling_convention_tests.rs